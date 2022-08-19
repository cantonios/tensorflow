# Copyright 2021 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Serialization Registration for SavedModel.

revived_types registration will be migrated to this infrastructure.
"""

from tensorflow.python.util import tf_inspect

_CLASS_REGISTRY = {}  # string registered name -> (predicate, class)
_REGISTERED_NAMES = []


def get_registered_name(obj):
  for name in reversed(_REGISTERED_NAMES):
    predicate, cls = _CLASS_REGISTRY[name]
    if not predicate and type(obj) == cls:  # pylint: disable=unidiomatic-typecheck
      return name
    if predicate and predicate(obj):
      return name
  return None


def get_registered_class(registered_name):
  try:
    return _CLASS_REGISTRY[registered_name][1]
  except KeyError:
    return None


def register_serializable(package="Custom", name=None, predicate=None):
  """Decorator for registering a serializable class.

  THIS METHOD IS STILL EXPERIMENTAL AND MAY CHANGE AT ANY TIME.

  Registered classes will be saved with a name generated by combining the
  `package` and `name` arguments. When loading a SavedModel, modules saved with
  this registered name will be created using the `_deserialize_from_proto`
  method.

  By default, only direct instances of the registered class will be saved/
  restored with the `serialize_from_proto`/`deserialize_from_proto` methods. To
  extend the registration to subclasses, use the `predicate argument`:

  ```python
  class A(tf.Module):
    pass

  register_serializable(
      package="Example", predicate=lambda obj: isinstance(obj, A))(A)
  ```

  Args:
    package: The package that this class belongs to.
    name: The name to serialize this class under in this package. If None, the
      class's name will be used.
    predicate: An optional function that takes a single Trackable argument, and
      determines whether that object should be serialized with this `package`
      and `name`. The default predicate checks whether the object's type exactly
      matches the registered class. Predicates are executed in the reverse order
      that they are added (later registrations are checked first).

  Returns:
    A decorator that registers the decorated class with the passed names and
    predicate.

  Raises:
    ValueError if predicate is not callable.
  """
  if predicate is not None and not callable(predicate):
    raise ValueError("The `predicate` passed to registered_serializable "
                     "must be callable.")

  def decorator(arg):
    """Registers a class with the serialization framework."""
    if not tf_inspect.isclass(arg):
      raise ValueError(
          "Registered serializable must be a class: {}".format(arg))

    class_name = name if name is not None else arg.__name__
    registered_name = package + "." + class_name

    if registered_name in _CLASS_REGISTRY:
      raise ValueError("{} has already been registered to {}".format(
          registered_name, _CLASS_REGISTRY[registered_name]))

    _CLASS_REGISTRY[registered_name] = (predicate, arg)
    _REGISTERED_NAMES.append(registered_name)

    return arg

  return decorator