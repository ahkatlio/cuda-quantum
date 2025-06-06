/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/
#include "py_state.h"
#include "LinkedLibraryHolder.h"
#include "common/ArgumentWrapper.h"
#include "common/Logger.h"
#include "cudaq/algorithms/get_state.h"
#include "utils/OpaqueArguments.h"
#include "mlir/Bindings/Python/PybindAdaptors.h"
#include "mlir/CAPI/IR.h"
#include <pybind11/complex.h>
#include <pybind11/stl.h>

namespace {
std::vector<int> bitStringToIntVec(const std::string &bitString) {
  // Check that this is a valid bit string.
  const bool isValidBitString =
      std::all_of(bitString.begin(), bitString.end(),
                  [](char c) { return c == '0' || c == '1'; });
  if (!isValidBitString)
    throw std::invalid_argument("Invalid bitstring: " + bitString);
  std::vector<int> result;
  result.reserve(bitString.size());
  for (const auto c : bitString)
    result.emplace_back(c == '0' ? 0 : 1);
  return result;
}
} // namespace

namespace cudaq {

void pyAltLaunchKernel(const std::string &, MlirModule, OpaqueArguments &,
                       const std::vector<std::string> &);

cudaq::KernelArgsHolder pyCreateNativeKernel(const std::string &, MlirModule,
                                             cudaq::OpaqueArguments &);

/// @brief If we have any implicit device-to-host data transfers
/// we will store that data here and ensure it is deleted properly.
std::vector<std::unique_ptr<void, std::function<void(void *)>>>
    hostDataFromDevice;

/// @brief Run `cudaq::get_state` on the provided kernel and spin operator.
state pyGetState(py::object kernel, py::args args) {
  if (py::hasattr(kernel, "compile"))
    kernel.attr("compile")();

  auto kernelName = kernel.attr("name").cast<std::string>();
  auto kernelMod = kernel.attr("module").cast<MlirModule>();
  auto *argData = toOpaqueArgs(args, kernelMod, kernelName);

  return details::extractState([&]() mutable {
    pyAltLaunchKernel(kernelName, kernelMod, *argData, {});
    delete argData;
  });
}

/// @brief Python implementation of the `RemoteSimulationState`.
// Note: Python kernel arguments are wrapped hence need to be unwrapped
// accordingly.
class PyRemoteSimulationState : public RemoteSimulationState {
  // Holder of args data for clean-up.
  cudaq::OpaqueArguments *argsData;
  mlir::ModuleOp kernelMod;

public:
  PyRemoteSimulationState(const std::string &in_kernelName,
                          cudaq::ArgWrapper args,
                          cudaq::OpaqueArguments *argsDataToOwn,
                          std::size_t size, std::size_t returnOffset)
      : argsData(argsDataToOwn), kernelMod(args.mod) {
    this->kernelName = in_kernelName;
    this->args = argsData->getArgs();
  }

  void execute() const override {
    if (!state) {
      auto &platform = cudaq::get_platform();
      // Create an execution context, indicate this is for
      // extracting the state representation
      ExecutionContext context("extract-state");
      // Perform the usual pattern set the context,
      // execute and then reset
      platform.set_exec_ctx(&context);
      // Note: in Python, the platform QPU (`PyRemoteSimulatorQPU`) expects an
      // ModuleOp pointer as the first element in the args array in StreamLined
      // mode.
      auto args = argsData->getArgs();
      args.insert(args.begin(),
                  const_cast<void *>(static_cast<const void *>(&kernelMod)));
      platform.launchKernel(kernelName, args);
      platform.reset_exec_ctx();
      state = std::move(context.simulationState);
    }
  }

  std::complex<double> overlap(const cudaq::SimulationState &other) override {
    const auto &otherState =
        dynamic_cast<const PyRemoteSimulationState &>(other);
    auto &platform = cudaq::get_platform();
    ExecutionContext context("state-overlap");
    context.overlapComputeStates = std::make_pair(
        static_cast<const cudaq::SimulationState *>(this),
        static_cast<const cudaq::SimulationState *>(&otherState));
    platform.set_exec_ctx(&context);
    auto args = argsData->getArgs();
    args.insert(args.begin(),
                const_cast<void *>(static_cast<const void *>(&kernelMod)));
    platform.launchKernel(kernelName, args);
    platform.reset_exec_ctx();
    assert(context.overlapResult.has_value());
    return context.overlapResult.value();
  }

  virtual ~PyRemoteSimulationState() override { delete argsData; }
};

/// @brief Run `cudaq::get_state` for remote execution targets on the provided
/// kernel and args
state pyGetStateRemote(py::object kernel, py::args args) {
  if (py::hasattr(kernel, "compile"))
    kernel.attr("compile")();

  auto kernelName = kernel.attr("name").cast<std::string>();
  args = simplifiedValidateInputArguments(args);
  auto kernelMod = kernel.attr("module").cast<MlirModule>();
  auto *argData = toOpaqueArgs(args, kernelMod, kernelName);
  auto [argWrapper, size, returnOffset] =
      pyCreateNativeKernel(kernelName, kernelMod, *argData);
  return state(new PyRemoteSimulationState(kernelName, argWrapper, argData,
                                           size, returnOffset));
}

/// @brief Python implementation of the `QPUState`.
// Note: Python kernel arguments are wrapped hence need to be unwrapped
// accordingly.
class PyQPUState : public QPUState {
  // Holder of args data for clean-up.
  cudaq::OpaqueArguments *argsData;

public:
  PyQPUState(const std::string &in_kernelName,
             cudaq::OpaqueArguments *argsDataToOwn)
      : argsData(argsDataToOwn) {
    this->kernelName = in_kernelName;
    this->args = argsData->getArgs();
  }

  virtual ~PyQPUState() override { delete argsData; }
};

/// @brief Run `cudaq::get_state` for qpu targets on the provided
/// kernel and args
state pyGetStateQPU(py::object kernel, py::args args) {
  if (py::hasattr(kernel, "compile"))
    kernel.attr("compile")();

  auto kernelName = kernel.attr("name").cast<std::string>();
  args = simplifiedValidateInputArguments(args);
  auto kernelMod = kernel.attr("module").cast<MlirModule>();
  auto *argData = toOpaqueArgs(args, kernelMod, kernelName);
  auto [argWrapper, size, returnOffset] =
      pyCreateNativeKernel(kernelName, kernelMod, *argData);
  return state(new PyQPUState(kernelName, argData));
}

state pyGetStateLibraryMode(py::object kernel, py::args args) {
  return details::extractState([&]() mutable {
    if (0 == args.size())
      kernel();
    else {
      std::vector<py::object> argsData;
      for (size_t i = 0; i < args.size(); i++) {
        py::object arg = args[i];
        argsData.emplace_back(std::forward<py::object>(arg));
      }
      kernel(std::move(argsData));
    }
  });
}

/// @brief Bind the get_state cudaq function
void bindPyState(py::module &mod, LinkedLibraryHolder &holder) {
  py::enum_<cudaq::InitialState>(mod, "InitialStateType",
                                 "Enumeration describing the initial state "
                                 "type to be created in the backend")
      .value("ZERO", cudaq::InitialState::ZERO)
      .value("UNIFORM", cudaq::InitialState::UNIFORM)
      .export_values();

  py::class_<SimulationState::Tensor>(
      mod, "Tensor",
      "The `Tensor` describes a pointer to simulation data as well as the rank "
      "and extents for that tensorial data it represents.")
      .def("data",
           [](SimulationState::Tensor &tensor) {
             return reinterpret_cast<intptr_t>(tensor.data);
           })
      .def_readonly("extents", &SimulationState::Tensor::extents)
      .def("get_rank", &SimulationState::Tensor::get_rank)
      .def("get_element_size", &SimulationState::Tensor::element_size)
      .def("get_num_elements", &SimulationState::Tensor::get_num_elements);
  py::class_<state>(
      mod, "State", py::buffer_protocol(),
      "A data-type representing the quantum state of the internal simulator. "
      "This type is not user-constructible and instances can only be retrieved "
      "via the `cudaq.get_state(...)` function or the static "
      "cudaq.State.from_data() method. \n")
      .def_buffer([](const state &self) -> py::buffer_info {
        if (self.get_num_tensors() != 1)
          throw std::runtime_error("Numpy interop is only supported for vector "
                                   "and matrix state data.");

        // This method is used by Pybind to enable interoperability
        // with NumPy array data. We therefore must be careful since the
        // state data may actually be on GPU device.

        // Get the data pointer.
        // Data may be on GPU device, if so we must make a copy to host.
        // If users do not want this copy, they will have to operate apart from
        // Numpy
        void *dataPtr = nullptr;
        auto stateVector = self.get_tensor();
        auto precision = self.get_precision();
        if (self.is_on_gpu()) {
          // This is device data, transfer to host, which gives us
          // ownership of a new data pointer on host. Store it globally
          // here so we ensure that it gets cleaned up.
          auto numElements = stateVector.get_num_elements();
          if (precision == SimulationState::precision::fp32) {
            auto *hostData = new std::complex<float>[numElements];
            self.to_host(hostData, numElements);
            dataPtr = reinterpret_cast<void *>(hostData);
          } else {
            auto *hostData = new std::complex<double>[numElements];
            self.to_host(hostData, numElements);
            dataPtr = reinterpret_cast<void *>(hostData);
          }
          hostDataFromDevice.emplace_back(dataPtr, [](void *data) {
            cudaq::info("freeing data that was copied from GPU device for "
                        "compatibility with NumPy");
            free(data);
          });
        } else
          dataPtr = self.get_tensor().data;

        // We need to know the precision of the simulation data
        // to get the data type size and the format descriptor
        auto [dataTypeSize, desc] =
            precision == SimulationState::precision::fp32
                ? std::make_tuple(
                      sizeof(std::complex<float>),
                      py::format_descriptor<std::complex<float>>::format())
                : std::make_tuple(
                      sizeof(std::complex<double>),
                      py::format_descriptor<std::complex<double>>::format());

        // Get the shape of the data. Return buffer info in a
        // correctly shaped manner.
        auto shape = self.get_tensor().extents;
        if (shape.size() != 1)
          return py::buffer_info(dataPtr, dataTypeSize, /*itemsize */
                                 desc, 2,               /* ndim */
                                 {shape[0], shape[1]},  /* shape */
                                 {dataTypeSize * static_cast<ssize_t>(shape[1]),
                                  dataTypeSize}, /* strides */
                                 true            /* readonly */
          );
        return py::buffer_info(dataPtr, dataTypeSize, /*itemsize */
                               desc, 1,               /* ndim */
                               {shape[0]},            /* shape */
                               {dataTypeSize});
      })
      .def(
          "__len__",
          [](state &self) {
            if (self.get_num_tensors() > 1 ||
                self.get_tensor().extents.size() != 1)
              throw std::runtime_error(
                  "len(state) only supported for state-vector like data.");

            return self.get_tensor().extents[0];
          },
          "For vector-like state data, return the number of state vector "
          "elements.")
      .def(
          "num_qubits", [](state &self) { return self.get_num_qubits(); },
          "Returns the number of qubits represented by this state.")
      .def_static(
          "from_data",
          [&](py::buffer data) {
            // This is by default host data
            auto info = data.request();
            if (info.format ==
                py::format_descriptor<std::complex<float>>::format()) {
              return state::from_data(std::make_pair(
                  reinterpret_cast<std::complex<float> *>(info.ptr),
                  info.size));
            }
            if (info.format ==
                py::format_descriptor<std::complex<double>>::format()) {
              return state::from_data(std::make_pair(
                  reinterpret_cast<std::complex<double> *>(info.ptr),
                  info.size));
            }
            throw std::runtime_error(
                "A numpy array with only floating point elements passed to "
                "state.from_data. input must be of complex float type, "
                "please "
                "add to your array creation `dtype=numpy.complex64` if "
                "simulation is FP32 and `dtype=numpy.complex128` if "
                "simulation if FP64, or dtype=cudaq.complex() for "
                "precision-agnostic code");
          },
          "Return a state from data.")
      .def_static(
          "from_data",
          [](const std::vector<py::buffer> &tensors) {
            cudaq::TensorStateData tensorData;
            for (auto &tensor : tensors) {
              auto info = tensor.request();
              const std::vector<std::size_t> extents(info.shape.begin(),
                                                     info.shape.end());
              tensorData.emplace_back(
                  std::pair<const void *, std::vector<std::size_t>>{info.ptr,
                                                                    extents});
            }
            return state::from_data(tensorData);
          },
          "Return a state from matrix product state tensor data.")
      .def_static(
          "from_data",
          [](const std::vector<SimulationState::Tensor> &tensors) {
            cudaq::TensorStateData tensorData;
            for (auto &tensor : tensors) {

              tensorData.emplace_back(
                  std::pair<const void *, std::vector<std::size_t>>{
                      tensor.data, tensor.extents});
            }
            return state::from_data(tensorData);
          },
          "Return a state from matrix product state tensor data.")
      .def_static(
          "from_data",
          [](const py::list &tensors) {
            // Note: we must use Python type (py::list) for proper overload
            // resolution. The overload for py::object, intended for cupy arrays
            // (implementing Python array interface), may be overshadowed by any
            // std::vector overloads.
            cudaq::TensorStateData tensorData;
            for (auto &tensor : tensors) {
              // Make sure this is a CuPy array
              if (!py::hasattr(tensor, "data"))
                throw std::runtime_error(
                    "invalid from_data operation on py::object - "
                    "only cupy array supported.");
              auto data = tensor.attr("data");
              if (!py::hasattr(data, "ptr"))
                throw std::runtime_error(
                    "invalid from_data operation on py::object tensors - "
                    "only cupy array supported.");

              // We know this is a cupy device pointer.
              // Start by ensuring it is of proper complex type
              auto typeStr = py::str(tensor.attr("dtype")).cast<std::string>();
              if (typeStr != "complex128")
                throw std::runtime_error(
                    "invalid from_data operation on py::object tensors - "
                    "only cupy complex128 tensors supported.");
              auto shape = tensor.attr("shape").cast<py::tuple>();
              std::vector<std::size_t> extents;
              for (auto el : shape)
                extents.emplace_back(el.cast<std::size_t>());
              long ptr = data.attr("ptr").cast<long>();
              tensorData.emplace_back(
                  std::pair<const void *, std::vector<std::size_t>>{
                      reinterpret_cast<std::complex<double> *>(ptr), extents});
            }
            return state::from_data(tensorData);
          },
          "Return a state from matrix product state tensor data (as CuPy "
          "ndarray).")
      .def_static(
          "from_data",
          [&holder](py::object opaqueData) {
            // Make sure this is a CuPy array
            if (!py::hasattr(opaqueData, "data"))
              throw std::runtime_error(
                  "invalid from_data operation on py::object - "
                  "only cupy array supported.");
            auto data = opaqueData.attr("data");
            if (!py::hasattr(data, "ptr"))
              throw std::runtime_error(
                  "invalid from_data operation on py::object - "
                  "only cupy array supported.");

            // We know this is a cupy device pointer.
            // Start by ensuring it is of complex type
            auto typeStr =
                py::str(opaqueData.attr("dtype")).cast<std::string>();
            if (typeStr.find("float") != std::string::npos)
              throw std::runtime_error(
                  "CuPy array with only floating point elements passed to "
                  "state.from_data. input must be of complex float type, "
                  "please "
                  "add to your cupy array creation `dtype=cupy.complex64` if "
                  "simulation is FP32 and `dtype=cupy.complex128` if "
                  "simulation if FP64.");

            // Compute the number of elements in the array
            std::vector<std::size_t> extents;
            auto numElements = [&]() {
              auto shape = opaqueData.attr("shape").cast<py::tuple>();
              std::size_t numElements = 1;
              for (auto el : shape) {
                numElements *= el.cast<std::size_t>();
                extents.emplace_back(el.cast<std::size_t>());
              }
              return numElements;
            }();

            long ptr = data.attr("ptr").cast<long>();
            if (holder.getTarget().name == "dynamics") {
              // For dynamics, we need to send on the extents to
              // distinguish state vector vs density matrix.
              cudaq::TensorStateData tensorData{
                  std::pair<const void *, std::vector<std::size_t>>{
                      reinterpret_cast<std::complex<double> *>(ptr), extents}};
              return state::from_data(tensorData);
            }

            // Check that the target is GPU-based, i.e., can handle device
            // pointer.
            if (!holder.getTarget().config.GpuRequired)
              throw std::runtime_error(fmt::format(
                  "Current target '{}' does not support CuPy arrays.",
                  holder.getTarget().name));

            if (typeStr == "complex64")
              return cudaq::state::from_data(std::make_pair(
                  reinterpret_cast<std::complex<float> *>(ptr), numElements));
            else if (typeStr == "complex128")
              return cudaq::state::from_data(std::make_pair(
                  reinterpret_cast<std::complex<double> *>(ptr), numElements));
            else
              throw std::runtime_error("invalid cupy element type " + typeStr);
          },
          "Return a state from CuPy device array.")
      .def("is_on_gpu", &cudaq::state::is_on_gpu,
           "Return True if this state is on the GPU.")
      .def(
          "getTensor",
          [](state &self, std::size_t idx) { return self.get_tensor(idx); },
          py::arg("idx") = 0,
          "Return the `idx` tensor making up this state representation.")
      .def(
          "getTensors", [](state &self) { return self.get_tensors(); },
          "Return all the tensors that comprise this state representation.")
      .def(
          "__getitem__",
          [](state &s, int idx) {
            // Support Pythonic negative index
            if (idx < 0)
              idx += (1 << s.get_num_qubits());
            return s[idx];
          },
          R"#(Return the `index`-th element of the state vector.
          
.. code-block:: python

  # Example:
  # Create a simple state vector.
  # Requires state-vector simulator
  state = cudaq.get_state(kernel)
  # Return the 0-th entry.
  value = state[0])#")
      .def(
          "__getitem__",
          [](state &s, std::vector<int> idx) {
            if (idx.size() != 2)
              throw std::runtime_error("Density matrix needs 2 indices; " +
                                       std::to_string(idx.size()) +
                                       " provided.");
            for (auto &val : idx)
              // Support Pythonic negative index
              if (val < 0)
                val += (1 << s.get_num_qubits());
            return s(idx[0], idx[1]);
          },
          R"#(Return the element of the density matrix at the provided
index pair.

.. code-block:: python

  # Example:
  # Create a simple density matrix.
  cudaq.set_target('density-matrix-cpu')
  densityMatrix = cudaq.get_state(kernel)
  # Return the upper-left most entry of the matrix.
  value = densityMatrix[0,0])#")
      .def(
          "amplitude",
          [](state &s, std::vector<int> basisState) {
            return s.amplitude(basisState);
          },
          R"#(Return the amplitude of a state in computational basis.
          
.. code-block:: python

  # Example:
  # Create a simulation state.
  state = cudaq.get_state(kernel)
  # Return the amplitude of |0101>, assuming this is a 4-qubit state.
  amplitude = state.amplitude([0,1,0,1]))#")
      .def(
          "amplitude",
          [](state &s, const std::string &bitString) {
            return s.amplitude(bitStringToIntVec(bitString));
          },
          R"#(Return the amplitude of a state in computational basis.
          
.. code-block:: python

  # Example:
  # Create a simulation state.
  state = cudaq.get_state(kernel)
  # Return the amplitude of |0101>, assuming this is a 4-qubit state.
  amplitude = state.amplitude('0101'))#")
      .def(
          "amplitudes",
          [](state &s, const std::vector<std::vector<int>> &basisStates) {
            return s.amplitudes(basisStates);
          },
          R"#(Return the amplitude of a list of states in computational basis.
          
.. code-block:: python

  # Example:
  # Create a simulation state.
  state = cudaq.get_state(kernel)
  # Return the amplitude of |0101> and |1010>, assuming this is a 4-qubit state.
  amplitudes = state.amplitudes([[0,1,0,1], [1,0,1,0]]))#")
      .def(
          "amplitudes",
          [](state &s, const std::vector<std::string> &bitStrings) {
            std::vector<std::vector<int>> basisStates;
            basisStates.reserve(bitStrings.size());
            for (const auto &bitString : bitStrings)
              basisStates.emplace_back(bitStringToIntVec(bitString));
            return s.amplitudes(basisStates);
          },
          R"#(Return the amplitudes of a list of states in computational basis.
          
.. code-block:: python

  # Example:
  # Create a simulation state.
  state = cudaq.get_state(kernel)
  # Return the amplitudes of |0101> and |1010>, assuming this is a 4-qubit state.
  amplitudes = state.amplitudes(['0101', '1010']))#")
      .def(
          "dump",
          [](state &self) {
            std::stringstream ss;
            self.dump(ss);
            py::print(ss.str());
          },
          "Print the state to the console.")
      .def("__str__",
           [](state &self) {
             std::stringstream ss;
             self.dump(ss);
             return ss.str();
           })
      .def(
          "overlap",
          [](state &self, state &other) { return self.overlap(other); },
          "Compute the overlap between the provided :class:`State`'s.")
      .def(
          "overlap",
          [](state &self, py::buffer &other) {
            if (self.get_num_tensors() != 1)
              throw std::runtime_error("overlap NumPy interop only supported "
                                       "for vector and matrix state data.");

            py::buffer_info info = other.request();

            if (info.shape.size() > 2)
              throw std::runtime_error("overlap NumPy interop only supported "
                                       "for vector and matrix state data.");

            // Check that the shapes are compatible
            std::size_t otherNumElements = 1;
            for (std::size_t i = 0; std::size_t shapeElement : info.shape) {
              otherNumElements *= shapeElement;
              if (shapeElement != self.get_tensor().extents[i++])
                throw std::runtime_error(
                    "overlap error - invalid shape of input buffer.");
            }

            // Compute the overlap in the case that the
            // input buffer is FP64
            if (info.itemsize == 16) {
              // if this state is FP32, then we have to throw an error
              if (self.get_precision() == SimulationState::precision::fp32)
                throw std::runtime_error(
                    "simulation state is FP32 but provided state buffer for "
                    "overlap is FP64.");

              auto otherState = state::from_data(std::make_pair(
                  reinterpret_cast<complex *>(info.ptr), otherNumElements));
              return self.overlap(otherState);
            }

            // Compute the overlap in the case that the
            // input buffer is FP32
            if (info.itemsize == 8) {
              // if this state is FP64, then we have to throw an error
              if (self.get_precision() == SimulationState::precision::fp64)
                throw std::runtime_error(
                    "simulation state is FP64 but provided state buffer for "
                    "overlap is FP32.");
              auto otherState = state::from_data(std::make_pair(
                  reinterpret_cast<std::complex<float> *>(info.ptr),
                  otherNumElements));
              return self.overlap(otherState);
            }

            // We only support complex f32 and f64 types
            throw std::runtime_error(
                "invalid buffer element type size for overlap computation.");
          },
          "Compute the overlap between the provided :class:`State`'s.")
      .def(
          "overlap",
          [](state &self, py::object other) {
            // Make sure this is a CuPy array
            if (!py::hasattr(other, "data"))
              throw std::runtime_error(
                  "invalid overlap operation on py::object - "
                  "only cupy array supported.");
            auto data = other.attr("data");
            if (!py::hasattr(data, "ptr"))
              throw std::runtime_error(
                  "invalid overlap operation on py::object - "
                  "only cupy array supported.");

            // We know this is a cupy device pointer.

            // Start by ensuring it is of complex type
            auto typeStr = py::str(other.attr("dtype")).cast<std::string>();
            if (typeStr.find("float") != std::string::npos)
              throw std::runtime_error(
                  "CuPy array with only floating point elements passed to "
                  "state.overlap. input must be of complex float type, please "
                  "add to your cupy array creation `dtype=cupy.complex64` if "
                  "simulation is FP32 and `dtype=cupy.complex128` if "
                  "simulation if FP64.");
            auto precision = self.get_precision();
            if (typeStr == "complex64") {
              if (precision == cudaq::SimulationState::precision::fp64)
                throw std::runtime_error(
                    "underlying simulation state is FP64, but "
                    "input cupy array is FP32.");
            } else if (typeStr == "complex128") {
              if (precision == cudaq::SimulationState::precision::fp32)
                throw std::runtime_error(
                    "underlying simulation state is FP32, but "
                    "input cupy array is FP64.");
            } else
              throw std::runtime_error("invalid cupy element type " + typeStr);

            // Compute the number of elements in the other array
            auto numOtherElements = [&]() {
              auto shape = other.attr("shape").cast<py::tuple>();
              std::size_t numElements = 1;
              for (auto el : shape)
                numElements *= el.cast<std::size_t>();
              return numElements;
            }();

            // Cast the device ptr and perform the overlap
            long ptr = data.attr("ptr").cast<long>();
            if (precision == SimulationState::precision::fp32)
              return self.overlap(cudaq::state::from_data(
                  std::make_pair(reinterpret_cast<std::complex<float> *>(ptr),
                                 numOtherElements)));

            return self.overlap(cudaq::state::from_data(
                std::make_pair(reinterpret_cast<std::complex<double> *>(ptr),
                               numOtherElements)));
          },
          "Compute overlap with general CuPy device array.");

  mod.def(
      "get_state",
      [&](py::object kernel, py::args args) {
        if (holder.getTarget().name == "remote-mqpu" ||
            holder.getTarget().name == "nvqc")
          return pyGetStateRemote(kernel, args);
        if (holder.getTarget().name == "orca-photonics")
          return pyGetStateLibraryMode(kernel, args);
        if (holder.getTarget().is_remote() || holder.getTarget().is_emulated())
          return pyGetStateQPU(kernel, args);
        return pyGetState(kernel, args);
      },
      R"#(Return the :class:`State` of the system after execution of the provided `kernel`.

Args:
  kernel (:class:`Kernel`): The :class:`Kernel` to execute on the QPU.
  *arguments (Optional[Any]): The concrete values to evaluate the kernel 
    function at. Leave empty if the kernel doesn't accept any arguments.

.. code-block:: python

  # Example:
  import numpy as np

  # Define a kernel that will produced the all |11...1> state.
  kernel = cudaq.make_kernel()
  qubits = kernel.qalloc(3)
  # Prepare qubits in the 1-state.
  kernel.x(qubits)

  # Get the state of the system. This will execute the provided kernel
  # and, depending on the selected target, will return the state as a
  # vector or matrix.
  state = cudaq.get_state(kernel)
  print(state))#");

  py::class_<async_state_result>(
      mod, "AsyncStateResult",
      R"#(A data-type containing the results of a call to :func:`get_state_async`. 
The `AsyncStateResult` models a future-like type, whose 
:class:`State` may be returned via an invocation of the `get` method. This 
kicks off a wait on the current thread until the results are available.
See `future <https://en.cppreference.com/w/cpp/thread/future>`_ 
for more information on this programming pattern.)#")
      .def(
          "get", [](async_state_result &self) { return self.get(); },
          py::call_guard<py::gil_scoped_release>(),
          "Return the :class:`State` from the asynchronous `get_state` "
          "accessor execution.\n");

  mod.def(
      "get_state_async",
      [](py::object kernel, py::args args, std::size_t qpu_id) {
        if (py::hasattr(kernel, "compile"))
          kernel.attr("compile")();

        auto kernelName = kernel.attr("name").cast<std::string>();
        auto kernelMod = kernel.attr("module").cast<MlirModule>();
        auto *argData = toOpaqueArgs(args, kernelMod, kernelName);

        // Launch the asynchronous execution.
        auto &platform = cudaq::get_platform();
        py::gil_scoped_release release;
        return details::runGetStateAsync(
            [kernelMod, argData, kernelName]() mutable {
              pyAltLaunchKernel(kernelName, kernelMod, *argData, {});
              delete argData;
            },
            platform, qpu_id);
      },
      py::arg("kernel"), py::kw_only(), py::arg("qpu_id") = 0,
      R"#(Asynchronously retrieve the state generated by the given quantum kernel. 
When targeting a quantum platform with more than one QPU, the optional
`qpu_id` allows for control over which QPU to enable. Will return a
future whose results can be retrieved via `future.get()`.

Args:
  kernel (:class:`Kernel`): The :class:`Kernel` to execute on the QPU.
  *arguments (Optional[Any]): The concrete values to evaluate the kernel 
    function at. Leave empty if the kernel doesn't accept any arguments.
  qpu_id (Optional[int]): The optional identification for which QPU 
    on the platform to target. Defaults to zero. Key-word only.

Returns:
  :class:`AsyncStateResult`: Quantum state (state vector or density matrix) data).)#");
}

} // namespace cudaq
