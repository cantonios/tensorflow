/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/sharded_device_array.h"

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#ifdef JAX_ENABLE_IFRT
#include "tensorflow/compiler/xla/python/ifrt/array.h"
#include "tensorflow/compiler/xla/python/ifrt/device.h"
#include "tensorflow/compiler/xla/python/ifrt/sharding.h"
#include "tensorflow/compiler/xla/python/pjrt_ifrt/pjrt_array.h"
#endif
#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/compiler/xla/python/py_buffer.h"
#include "tensorflow/compiler/xla/python/python_utils.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace jax {

namespace py = pybind11;

namespace {

struct ShardedDeviceArrayBaseObject {
  PyObject_HEAD;
};
static_assert(std::is_standard_layout<ShardedDeviceArrayBaseObject>::value,
              "ShardedDeviceArrayBaseObject must be standard layout");

struct ShardedDeviceArrayObject {
  ShardedDeviceArrayBaseObject base;
  ShardedDeviceArray sda;
  // Used by the Python interpreter to maintain a list of weak references to
  // this object.
  PyObject* weakrefs;
};
static_assert(std::is_standard_layout<ShardedDeviceArrayObject>::value,
              "ShardedDeviceArrayObject must be standard layout");

PyObject* sharded_device_array_tp_new(PyTypeObject* subtype, PyObject* args,
                                      PyObject* kwds) {
  ShardedDeviceArrayObject* self = reinterpret_cast<ShardedDeviceArrayObject*>(
      subtype->tp_alloc(subtype, 0));
  if (!self) return nullptr;
  self->weakrefs = nullptr;
  return reinterpret_cast<PyObject*>(self);
}

void sharded_device_array_tp_dealloc(PyObject* self) {
  PyTypeObject* tp = Py_TYPE(self);
  ShardedDeviceArrayObject* o =
      reinterpret_cast<ShardedDeviceArrayObject*>(self);
  if (o->weakrefs) {
    PyObject_ClearWeakRefs(self);
  }
  o->sda.~ShardedDeviceArray();
  tp->tp_free(self);
  Py_DECREF(tp);
}

}  // namespace

void ShardedDeviceArray::Delete() {
  // If already deleted, do nothing.
  if (is_deleted_) {
    return;
  }
#ifdef JAX_ENABLE_IFRT
  auto array = ifrt_array();
  if (!array.ok()) {
    return;
  }
  ifrt_array_ = std::nullopt;
#else
  // We can't inline this expression into the for loop! Here, .value()
  // returns an rvalue reference to the Span embedded in the StatusOr.
  // Binding the reference would extend the lifetime of the Span itself,
  // but not of the StatusOr, causing stack-use-after-scope errors. Also see
  // https://en.cppreference.com/w/cpp/language/range-for#Temporary_range_expression
  auto buffers = pjrt_buffers().value();
  for (xla::PjRtBuffer* pjrt_buffer : buffers) {
    pjrt_buffer->Delete();
  }
#endif
  device_buffers_ = std::nullopt;
  cpp_device_buffers_ = std::nullopt;
  npy_value_ = std::nullopt;
  is_deleted_ = true;
}

#ifdef JAX_ENABLE_IFRT
xla::StatusOr<xla::ifrt::Array*> ShardedDeviceArray::ifrt_array() {
  if (ifrt_array_.has_value()) {
    return ifrt_array_->get();
  }
  if (!device_buffers_.has_value()) {
    return xla::InvalidArgument("ShardedDeviceArray has been deleted.");
  }
  const int num_devices = device_buffers_->size();
  std::vector<xla::ifrt::Array*> ifrt_arrays;
  ifrt_arrays.reserve(num_devices);
  std::vector<xla::ifrt::Shape> shapes;
  shapes.reserve(num_devices);
  xla::ifrt::DeviceList::Devices devices;
  devices.reserve(num_devices);
  for (auto& handle : device_buffers_.value()) {
    // Note that invariants guarantee the cast should never fail.
    TF_ASSIGN_OR_RETURN(xla::PyBuffer * pybuffer,
                        xla::PyBuffer::AsPyBuffer(handle));
    ifrt_arrays.push_back(pybuffer->ifrt_array());
    shapes.push_back(pybuffer->ifrt_array()->shape());
    devices.push_back(pybuffer->ifrt_array()->sharding().devices().front());
  }
  xla::ifrt::Client* client = ifrt_arrays.front()->client();
  xla::ifrt::Shape shape(
      pybind11::cast<std::vector<int64_t>>(aval_.attr("shape")));
  auto sharding = xla::ifrt::OpaqueSharding::Create(
      xla::ifrt::DeviceList(std::move(devices)),
      xla::ifrt::OpaqueSharding::MakeDisassembleFuncFromShapes(
          std::move(shapes)));
  TF_ASSIGN_OR_RETURN(auto ifrt_array,
                      client->AssembleArrayFromSingleDeviceArrays(
                          std::move(shape), std::move(sharding), ifrt_arrays,
                          xla::ifrt::ArrayCopySemantics::kReuseInput));
  ifrt_array_ = std::move(ifrt_array);
  return ifrt_array_->get();
}
#endif

xla::StatusOr<absl::Span<xla::PjRtBuffer* const>>
ShardedDeviceArray::pjrt_buffers() {
  if (cpp_device_buffers_.has_value()) {
    return absl::MakeConstSpan(*cpp_device_buffers_);
  }

#ifdef JAX_ENABLE_IFRT
  TF_ASSIGN_OR_RETURN(auto* ifrt_array, ifrt_array());
  auto* pjrt_array = llvm::dyn_cast_or_null<xla::ifrt::PjRtArray>(ifrt_array);
  if (pjrt_array == nullptr) {
    throw xla::XlaRuntimeError(
        "This operation is implemented for a PjRt-compatible backend only.");
  }
  const int num_devices = device_buffers_->size();
  std::vector<xla::PjRtBuffer*> cpp_device_buffers;
  cpp_device_buffers.reserve(num_devices);
  for (const auto& pjrt_buffer : pjrt_array->pjrt_buffers()) {
    cpp_device_buffers.push_back(pjrt_buffer.get());
  }
#else
  if (!device_buffers_.has_value()) {
    return xla::InvalidArgument("ShardedDeviceArray has been deleted.");
  }
  const int num_devices = device_buffers_->size();
  std::vector<xla::PjRtBuffer*> cpp_device_buffers;
  cpp_device_buffers.reserve(num_devices);
  int i = 0;
  for (auto& handle : device_buffers_.value()) {
    // Note that invariants guarantee the cast should never fail.
    TF_ASSIGN_OR_RETURN(xla::PyBuffer * pybuffer,
                        xla::PyBuffer::AsPyBuffer(handle));
    cpp_device_buffers.push_back(pybuffer->pjrt_buffer());
    i += 1;
  }
#endif
  cpp_device_buffers_ = std::move(cpp_device_buffers);
  return absl::MakeConstSpan(cpp_device_buffers_.value());
}

PyObject* ShardedDeviceArray::base_type_ = nullptr;
PyObject* ShardedDeviceArray::type_ = nullptr;

/*static*/ ShardedDeviceArray::object ShardedDeviceArray::Make(
    py::object aval, ShardingSpec sharding_spec, py::list device_buffers,
    py::object indices, bool weak_type) {
  py::object obj =
      py::reinterpret_steal<py::object>(sharded_device_array_tp_new(
          reinterpret_cast<PyTypeObject*>(type_), nullptr, nullptr));
  ShardedDeviceArrayObject* sda =
      reinterpret_cast<ShardedDeviceArrayObject*>(obj.ptr());
  new (&sda->sda)
      ShardedDeviceArray(aval, std::move(sharding_spec),
                         std::move(device_buffers), indices, weak_type);
  return py::reinterpret_borrow<ShardedDeviceArray::object>(obj);
}

/*static*/ ShardedDeviceArray::object ShardedDeviceArray::Make(
    py::object aval, ShardingSpec sharding_spec,
    const xla::PyShardedBuffer& sharded_buffer, py::object indices,
    bool weak_type) {
  int num_devices = sharded_buffer.num_devices();
  py::list device_buffers(num_devices);
  for (int i = 0; i < num_devices; ++i) {
    device_buffers[i] = sharded_buffer.GetPyBuffer(i);
  }
  return Make(std::move(aval), std::move(sharding_spec),
              std::move(device_buffers), std::move(indices), weak_type);
}

bool ShardedDeviceArray::IsShardedDeviceArray(py::handle handle) {
  return handle.get_type() == ShardedDeviceArray::type();
}

/*static*/ ShardedDeviceArray*
ShardedDeviceArray::AsShardedDeviceArrayUnchecked(py::handle handle) {
  return &(reinterpret_cast<ShardedDeviceArrayObject*>(handle.ptr())->sda);
}

/*static*/ xla::StatusOr<ShardedDeviceArray*>
ShardedDeviceArray::AsShardedDeviceArray(py::handle handle) {
  if (!IsShardedDeviceArray(handle)) {
    return xla::InvalidArgument("Expected a ShardedDeviceArray");
  }
  return AsShardedDeviceArrayUnchecked(handle);
}

py::handle ShardedDeviceArray::AsHandle() {
  return reinterpret_cast<PyObject*>(reinterpret_cast<char*>(this) -
                                     offsetof(ShardedDeviceArrayObject, sda));
}

/*static*/ xla::Status ShardedDeviceArray::RegisterTypes(py::module& m) {
  // We need to use heap-allocated type objects because we want to add
  // additional methods dynamically.
  // Similar to py_buffer.cc
  {
    py::str name = py::str("ShardedDeviceArrayBase");
    py::str qualname = py::str("ShardedDeviceArrayBase");
    PyHeapTypeObject* heap_type = reinterpret_cast<PyHeapTypeObject*>(
        PyType_Type.tp_alloc(&PyType_Type, 0));
    // Caution: we must not call any functions that might invoke the GC until
    // PyType_Ready() is called. Otherwise the GC might see a half-constructed
    // type object.
    if (!heap_type) {
      return xla::Internal("Unable to create heap type object");
    }
    heap_type->ht_name = name.release().ptr();
    heap_type->ht_qualname = qualname.release().ptr();
    PyTypeObject* type = &heap_type->ht_type;
    type->tp_name = "ShardedDeviceArrayBase";
    type->tp_basicsize = sizeof(ShardedDeviceArrayBaseObject);
    type->tp_flags =
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_BASETYPE;
    TF_RET_CHECK(PyType_Ready(type) == 0);
    base_type_ = reinterpret_cast<PyObject*>(type);
  }
  py::object base_type = py::reinterpret_borrow<py::object>(base_type_);
  base_type.attr("__module__") = m.attr("__name__");
  m.attr("ShardedDeviceArrayBase") = base_type;

  {
    py::tuple bases = py::make_tuple(base_type);
    py::str name = py::str("ShardedDeviceArray");
    py::str qualname = py::str("ShardedDeviceArray");
    PyHeapTypeObject* heap_type = reinterpret_cast<PyHeapTypeObject*>(
        PyType_Type.tp_alloc(&PyType_Type, 0));
    // Caution: we must not call any functions that might invoke the GC until
    // PyType_Ready() is called below. Otherwise the GC might see a
    // half-constructed type object.
    if (!heap_type) {
      return xla::Internal("Unable to create heap type object");
    }
    heap_type->ht_name = name.release().ptr();
    heap_type->ht_qualname = qualname.release().ptr();
    PyTypeObject* type = &heap_type->ht_type;
    type->tp_name = "ShardedDeviceArray";
    type->tp_basicsize = sizeof(ShardedDeviceArrayObject);
    type->tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE;
    type->tp_bases = bases.release().ptr();
    type->tp_dealloc = sharded_device_array_tp_dealloc;
    type->tp_new = sharded_device_array_tp_new;
    // Supported protocols
    type->tp_as_number = &heap_type->as_number;
    type->tp_as_sequence = &heap_type->as_sequence;
    type->tp_as_mapping = &heap_type->as_mapping;
    type->tp_as_buffer = nullptr;

    // Allow weak references to DeviceArray objects.
    type->tp_weaklistoffset = offsetof(ShardedDeviceArrayObject, weakrefs);

    TF_RET_CHECK(PyType_Ready(type) == 0);
    type_ = reinterpret_cast<PyObject*>(type);
  }
  py::object type = py::reinterpret_borrow<py::object>(type_);
  type.attr("__module__") = m.attr("__name__");
  m.attr("ShardedDeviceArray") = type;

  type.attr("make") = def_static([](py::object aval, ShardingSpec sharding_spec,
                                    py::object sharded_buffer_or_device_buffers,
                                    py::object indices, bool weak_type) {
    // Overloads this "make" method to accept either a list of PyBuffers or a
    // PyShardedBuffer.
    if (py::isinstance<py::list>(sharded_buffer_or_device_buffers)) {
      return ShardedDeviceArray::Make(
          aval, sharding_spec,
          std::move(sharded_buffer_or_device_buffers).cast<py::list>(), indices,
          weak_type);
    }
    const auto& sharded_buffer =
        *sharded_buffer_or_device_buffers.cast<xla::PyShardedBuffer*>();

    return ShardedDeviceArray::Make(aval, sharding_spec, sharded_buffer,
                                    indices, weak_type);
  });
  type.attr("aval") =
      property_readonly([](ShardedDeviceArray::object self) -> py::object {
        return self.sda()->aval();
      });
  type.attr("indices") =
      property_readonly([](ShardedDeviceArray::object self) -> py::object {
        return self.sda()->indices();
      });
  type.attr("sharding_spec") =
      property_readonly([](ShardedDeviceArray::object self) {
        return self.sda()->GetShardingSpec();
      });
  type.attr("device_buffers") =
      property_readonly([](ShardedDeviceArray::object self) {
        return self.sda()->device_buffers();
      });
  type.attr("_npy_value") = property(
      [](ShardedDeviceArray::object self) { return self.sda()->npy_value(); },
      [](ShardedDeviceArray::object self, py::object npy_value) {
        return self.sda()->set_npy_value(npy_value);
      });
  type.attr("_one_replica_buffer_indices") = property(
      [](ShardedDeviceArray::object self) {
        return self.sda()->one_replica_buffer_indices();
      },
      [](ShardedDeviceArray::object self, py::object obj) {
        return self.sda()->set_one_replica_buffer_indices(obj);
      });
  type.attr("shape") = property_readonly([](ShardedDeviceArray::object self) {
    return self.sda()->aval().attr("shape");
  });
  type.attr("dtype") = property_readonly([](ShardedDeviceArray::object self) {
    return self.sda()->aval().attr("dtype");
  });
  type.attr("size") = property_readonly([](ShardedDeviceArray::object self) {
    py::tuple shape = py::cast<py::tuple>(self.sda()->aval().attr("shape"));
    int64_t size = 1;
    for (auto dim : shape) {
      size *= py::cast<int64_t>(dim);
    }
    return size;
  });
  type.attr("ndim") = property_readonly([](ShardedDeviceArray::object self) {
    return py::len(self.sda()->aval().attr("shape"));
  });

  type.attr("delete") = py::cpp_function(
      [](ShardedDeviceArray::object self) { self.sda()->Delete(); },
      py::is_method(type));
  type.attr("is_deleted") = py::cpp_function(
      [](ShardedDeviceArray::object self) { return self.sda()->is_deleted(); },
      py::is_method(type));

  return ::tsl::OkStatus();
}

}  // namespace jax
