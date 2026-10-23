# data/neural

`oa-data-neural` runs a learned network. A brain's `nets/*.oann` file is read
by `oa-formats-oann`; this module builds that model into buffers and runs it
on int16 inputs. The arithmetic is the format's, in
`src/formats/oann/README.md`. The same inputs give the same outputs on every
compiler and processor, including Visual Studio, GCC and Clang.

A run is charged by its operations, one charge unit per 64. The charge is
fixed when the network is built, because the shapes are fixed in the file.
One brain's networks together stay within 64 MiB.

## Entry points

`oa/data/neural/arithmetic.hpp` and `oa/data/neural/network.hpp` (namespace
`oa::data::neural`):

- `rescale` moves an integer from one exponent to another. `saturate_int16`
  and `saturate_int32` hold a 64-bit value to those ranges. Callers that
  prepare a network's inputs use `rescale`.
- `operations_per_charge` is 64. `max_operations_per_run` is 2^28.
  `max_brain_network_bytes` is 64 MiB.
- `Network::build` checks the model and refuses a network of more than
  `max_operations_per_run` operations. Every format-1 kind runs. A refusal
  is a `formats::oann::Problem`.
- `memory_bytes` and `memory_bytes_of` count constants at their own type's
  size and every input, activation and output at its type's size.
  `operations` and `charge_units` are the cost of one run.
- `inputs` and `outputs` are the ports. `input` is the span the caller fills.
  `run` writes every activation and output. `output` is an int16 result;
  `output_index` is an argmax, 0-based.
- `NetworkSet` holds networks under string ids, up to a byte cap. `add`
  refuses a repeated id and a network that would pass the cap. `find` returns
  the network.

## State

A network owns one buffer per input, activation and output, and native arrays
of its constants. The caller fills an input span, then `run` reads those
inputs and writes the activations and outputs. One run at a time per network.
A `NetworkSet` owns the networks it accepted. Two sets share nothing.

## Invariants

- Integers only. No fractional value is stored or computed.
- Sums are 64-bit. An allowed network cannot overflow a sum, so the order of
  the additions does not change a bit. A product is formed in 64 bits from
  widened operands. Shifting a negative value right keeps the sign.
- `run` allocates nothing and never throws. It does not read a clock and it
  does not start a thread.
- The same inputs give the same output bits on every build.
- The operation count, the charge and the memory are fixed by the file. They
  are not read from an allocator.

## Tests

`data-neural` builds small networks with `formats::oann::write`, reads them
back, and checks each layer kind against values worked by hand. A mixed
network, filled from a fixed sequence, is checked against
`tests/neural_reference.py`. Run that script with:

```sh
python3 -I src/data/neural/tests/neural_reference.py
```

Its printout, from `N02-NEURAL-REFERENCE-BEGIN` through
`N02-NEURAL-REFERENCE-END`, and from `N06-NEURAL-REFERENCE-BEGIN` through
`N06-NEURAL-REFERENCE-END`, is the table in the test. The first block is the
mixed network. The second is a grid: convolution, relu, pooling, a second
convolution, tanh, flatten, dense, sigmoid, then softmax beside argmax, on
16 pictures. The SHA-256 of each network's outputs is pinned. Each is the
same on every build and moves only when the arithmetic changes.

`tests/tables_reference.py` prints `src/data/neural/src/tables.inc`, the tanh,
sigmoid and exponent tables. That file is the script's output and is never
edited by hand. Run it with:

```sh
python3 -I src/data/neural/tests/tables_reference.py
```

The test pins the SHA-256 of each table's little-endian bytes, and the
entries at 0, 1, 2047, 2048, 2049, 4094 and 4095. Convolution and pooling
are also checked against values worked by hand.

## Limitations

- Single-threaded. One run at a time on a network, with no internal lock.
- No layer fusion. Each layer runs on its own.
- Each seat holds its own copy of a network.
