Noise Modeling with CUDA-Q
********************************

CUDA-Q models incoherent noise via quantum channels, linear 
completely-positive and trace-preserving (CPTP) maps. Specifically 
CUDA-Q requires the definition of a quantum channel via Kraus operators
that satisfy the CPTP condition (:math:`\sum_i K_i^\dagger K_i = \mathbf{I}`). 

Noise channels are composed of a list of Kraus operators and can be 
assigned for application after the application of a quantum operation. These 
channels can be stacked - multiple noise processes can be appended 
for a given quantum operation. These channels are specified to be applied to 
one or many qubits for a given operation. A Kraus channel should be able to 
be applied to a single qubit operation, or a multi-qubit, controlled operation. 

These channels transform the underlying density matrix representation of the quantum 
state in the following way 

.. math::

    \rho_{new} = \sum_i K_i \rho K_i^\dagger. 

Design 
------

To model this in CUDA-Q, we put forward :code:`cudaq::kraus_op`, :code:`cudaq::kraus_channel`, 
and :code:`cudaq::noise_model`. 

A :code:`cudaq::kraus_op` encapsulates a :code:`vector` of :code:`complex` 
data modeling a 1-D flatten representation of a matrix with row-major ordering. It 
can be constructed from a :code:`vector` of data or an :code:`initializer_list` of data.

A :code:`cudaq::kraus_channel` encapsulates a :code:`vector` of :code:`kraus_op` instances that 
make up the quantum channel. The size of a :code:`kraus_channel` 
is the number of :code:`kraus_ops` it contains. :code:`kraus_channel` instances can be constructed 
from a variadic list of :code:`initializer_list` that enables :code:`kraus_op` construction in place. 
It can also be constructed directly from a vector of :code:`kraus_ops`. The 
constructor should validate the completeness (CPTP) relation. 

A :code:`cudaq::noise_model` encapsulates a mapping of quantum operation names to a 
vector of :code:`kraus_channel` that is to be applied after invocation of that 
quantum operation. A :code:`noise_model` can be constructed with a nullary constructor, and 
:code:`kraus_channels` can be added via :code:`add_channel` and :code:`add_all_qubit_channel` methods with 
the operation given as a string or as a template argument. 
The operation name or the template type specifies the quantum operation the channel applies to 
(e.g. :code:`model.add_channel\<cudaq::types::h\>(channel)` or :code:`model.add_channel("h", channel)`). 
Clients (e.g. simulator backends) can retrieve the :code:`kraus_channel` to 
apply to the simulated state via a :code:`noise_model::get_channel(...)` call. 

When adding an error channel to a noise model for a quantum operation
we can assign the noise channel to instances of that operation on specific qubit operands or 
to any occurrence of the operation, regardless of which qubits it acts on. 

.. tab:: Python

    .. literalinclude:: /../snippets/python/noise/noise_model_examples.py
       :language: python
       :start-after: [Begin PY AddChannelSpecific]
       :end-before: [End PY AddChannelSpecific]
       :dedent: 8

    .. literalinclude:: /../snippets/python/noise/noise_model_examples.py
       :language: python
       :start-after: [Begin PY AddChannelAllQubit]
       :end-before: [End PY AddChannelAllQubit]
       :dedent: 8


.. tab:: C++

    .. literalinclude:: /../snippets/cpp/noise/noise_model_examples.cpp
       :language: cpp
       :start-after: [Begin CPP AddChannelSpecific]
       :end-before: [End CPP AddChannelSpecific]
       :dedent: 6
        
    .. literalinclude:: /../snippets/cpp/noise/noise_model_examples.cpp
       :language: cpp
       :start-after: [Begin CPP AddChannelAllQubit]
       :end-before: [End CPP AddChannelAllQubit]
       :dedent: 6

In addition to static noise channels, users can also define a noise channel as a 
callback function, which returns a concrete channel definition in terms of Kraus matrices 
depending on the gate operands and gate parameters if any.

.. tab:: Python

    .. literalinclude:: /../snippets/python/noise/noise_model_examples.py
       :language: python
       :start-after: [Begin PY AddChannelDynamic]
       :end-before: [End PY AddChannelDynamic]
       :dedent: 8


.. tab:: C++

    .. literalinclude:: /../snippets/cpp/noise/noise_model_examples.cpp
       :language: cpp
       :start-after: [Begin CPP AddChannelDynamic]
       :end-before: [End CPP AddChannelDynamic]
       :dedent: 6


Noise models can be constructed via the :code:`cudaq::noise_model` and specified for 
execution via a public :code:`cudaq::set_noise(cudaq::noise_model&)` function. This function 
should forward the :code:`noise_model` to the current :code:`quantum_platform` which can attach it 
to the current :code:`ExecutionContext` and retrieved by backend simulators. The
:code:`noise_model` must stay in scope in order to be successfully used by the
backend simulators, and you must call :code:`cudaq::unset_noise()` when you are
done with the noise model.

The :code:`kraus_op` matrix data assumes a LSB qubit ordering. 

To get started with noise modeling in CUDA-Q, take a look at the :doc:`/using/examples`.
