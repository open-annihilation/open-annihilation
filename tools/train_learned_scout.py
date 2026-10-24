#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Train the learned scout network and write it as an ONNX model.

Use:
    python3 -I tools/train_learned_scout.py RECORDS.jsonl --out MODEL.onnx

Input:
    A brain record file from --brain-record. Each line is one JSON object.
    Lines whose kind is not sample are ignored. A sample carries
    features.sectors, a tensor of shape [2, 8, 8] whose values are counts
    of 65536ths, and decision, the chosen sector as a whole number from 0.

Output:
    MODEL.onnx, a model with input sectors of shape [1, 2, 8, 8] and output
    choice. The network is conv2d 1x1 from 2 channels to 8, relu, conv2d
    1x1 from 8 channels to 1, flatten, and argmax. Two lines are printed:
        held-out agreement: NN.N% (correct/total)
        closed form agreement: NN.N% (correct/total)
    The first is how often the written model's argmax matches the recorded
    decision on the held-out samples. The second scores those same samples
    with a fixed ranking of explored share and distance. Those weights are
    not the trained network.

    The records are the learned scout's rule. A centre the unit cannot
    stand on is fully explored in channel 1, so the rule skips it by the
    explored share. The brain records the choice every 30 ticks. It sends
    the unit when the head order is absent or its idle bit is set.

Reproducibility:
    The same records produce the same model bytes on any machine.
    random.Random(20261023) draws a starting weight for every value,
    uniform in ±0.1. Gradient descent uses no further draws. It runs for
    550 epochs at rate 0.008, samples in file order, one update from the
    sum of the per-sample gradients. Every fifth sample, counting from 1,
    is held out and is not part of that sum. Sums are loops in a fixed
    order. The only arithmetic is +, -, * and /, plus comparisons. The
    weights are rounded to float32 before the agreement is measured and
    before the file is written.
"""

import argparse
import json
import random
import struct
import sys

HIDDEN = 8
CELLS = 64
CHANNELS = 2
# The best plain descent found on these records. A higher rate diverges,
# and a longer run at this rate lowers the held-out agreement.
EPOCHS = 550
RATE = 0.008
SEED = 20261023
SCALE = 0.1
UNIT = 65536.0
HALF = 0.5
WIDTH = 0.03125
SLOPE = 192.0
DISTANCE = 2.0
GATE = 32.0


def relu(value):
    if value > 0.0:
        return value
    return 0.0


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def closed_form():
    """The rule as one score per sector.

    Below half explored the score is LIFT - DISTANCE * distance. At or past
    half it is DISTANCE * distance. LIFT is SLOPE * WIDTH, which is larger
    than twice DISTANCE, so every open sector outranks every covered one.
    Among open sectors a nearer centre scores higher. Among covered sectors
    a farther centre scores higher. The lowest index wins a tie, because
    argmax keeps the first maximum.
    """
    weights_1 = [[0.0, 0.0] for _unit in range(HIDDEN)]
    bias_1 = [0.0 for _unit in range(HIDDEN)]
    weights_2 = [0.0 for _unit in range(HIDDEN)]
    bias_2 = 0.0
    weights_1[0] = [0.0, 1.0]
    bias_1[0] = 0.0
    weights_2[0] = -DISTANCE
    weights_1[1] = [-1.0, 0.0]
    bias_1[1] = HALF
    weights_2[1] = SLOPE
    weights_1[2] = [-1.0, 0.0]
    bias_1[2] = HALF - WIDTH
    weights_2[2] = -SLOPE
    weights_1[3] = [GATE, 1.0]
    bias_1[3] = -HALF * GATE
    weights_2[3] = 2.0 * DISTANCE
    weights_1[4] = [GATE, 0.0]
    bias_1[4] = -HALF * GATE
    weights_2[4] = -2.0 * DISTANCE
    return weights_1, bias_1, weights_2, bias_2


def starting_weights(rng):
    weights_1 = []
    bias_1 = []
    for _unit in range(HIDDEN):
        weights_1.append([
            (rng.random() * 2.0 - 1.0) * SCALE,
            (rng.random() * 2.0 - 1.0) * SCALE,
        ])
        bias_1.append((rng.random() * 2.0 - 1.0) * SCALE)
    weights_2 = [(rng.random() * 2.0 - 1.0) * SCALE for _unit in range(HIDDEN)]
    bias_2 = (rng.random() * 2.0 - 1.0) * SCALE
    return weights_1, bias_1, weights_2, bias_2


def round_weights(weights_1, bias_1, weights_2, bias_2):
    for unit in range(HIDDEN):
        weights_1[unit][0] = f32(weights_1[unit][0])
        weights_1[unit][1] = f32(weights_1[unit][1])
        bias_1[unit] = f32(bias_1[unit])
        weights_2[unit] = f32(weights_2[unit])
    return weights_1, bias_1, weights_2, f32(bias_2)


def score_at(explored, distance, weights_1, bias_1, weights_2, bias_2):
    score = bias_2
    for unit in range(HIDDEN):
        total = bias_1[unit]
        total = total + weights_1[unit][0] * explored
        total = total + weights_1[unit][1] * distance
        score = score + weights_2[unit] * relu(total)
    return score


def argmax(features, weights_1, bias_1, weights_2, bias_2):
    best = 0
    best_score = score_at(features[0], features[CELLS], weights_1, bias_1, weights_2, bias_2)
    for index in range(1, CELLS):
        score = score_at(
            features[index], features[CELLS + index], weights_1, bias_1, weights_2, bias_2
        )
        if score > best_score:
            best = index
            best_score = score
    return best


def train(samples):
    rng = random.Random(SEED)
    weights_1, bias_1, weights_2, bias_2 = starting_weights(rng)
    training = []
    held = []
    number = 0
    for features, decision in samples:
        number = number + 1
        if number % 5 == 0:
            held.append((features, decision))
        else:
            training.append((features, decision))
    count = 0
    for _sample in training:
        count = count + 1
    if count == 0:
        raise SystemExit("train_learned_scout: the record file has no training sample")
    for _epoch in range(EPOCHS):
        grad_w1 = [[0.0, 0.0] for _unit in range(HIDDEN)]
        grad_b1 = [0.0 for _unit in range(HIDDEN)]
        grad_w2 = [0.0 for _unit in range(HIDDEN)]
        grad_b2 = 0.0
        for features, decision in training:
            for index in range(CELLS):
                explored = features[index]
                distance = features[CELLS + index]
                pre = []
                hidden = []
                for unit in range(HIDDEN):
                    total = bias_1[unit]
                    total = total + weights_1[unit][0] * explored
                    total = total + weights_1[unit][1] * distance
                    pre.append(total)
                    hidden.append(relu(total))
                score = bias_2
                for unit in range(HIDDEN):
                    score = score + weights_2[unit] * hidden[unit]
                target = 0.0
                if index == decision:
                    target = 1.0
                delta = 2.0 * (score - target)
                grad_b2 = grad_b2 + delta
                for unit in range(HIDDEN):
                    grad_w2[unit] = grad_w2[unit] + delta * hidden[unit]
                    if pre[unit] > 0.0:
                        back = delta * weights_2[unit]
                        grad_b1[unit] = grad_b1[unit] + back
                        grad_w1[unit][0] = grad_w1[unit][0] + back * explored
                        grad_w1[unit][1] = grad_w1[unit][1] + back * distance
        for unit in range(HIDDEN):
            weights_1[unit][0] = weights_1[unit][0] - RATE * grad_w1[unit][0] / count
            weights_1[unit][1] = weights_1[unit][1] - RATE * grad_w1[unit][1] / count
            bias_1[unit] = bias_1[unit] - RATE * grad_b1[unit] / count
            weights_2[unit] = weights_2[unit] - RATE * grad_w2[unit] / count
        bias_2 = bias_2 - RATE * grad_b2 / count
    weights_1, bias_1, weights_2, bias_2 = round_weights(weights_1, bias_1, weights_2, bias_2)
    correct, total = count_correct(held, weights_1, bias_1, weights_2, bias_2)
    return weights_1, bias_1, weights_2, bias_2, correct, total, held


def whole_number(value, where):
    if isinstance(value, bool) or not isinstance(value, int):
        raise SystemExit("train_learned_scout: " + where + " is not a whole number")
    return value


def read_records(path):
    try:
        handle = open(path, "r", encoding="utf-8")
    except OSError as error:
        raise SystemExit("train_learned_scout: cannot read " + path + ": " + str(error))
    samples = []
    with handle:
        first = True
        line_number = 0
        for line in handle:
            line_number = line_number + 1
            if first:
                first = False
                if line.startswith("\ufeff"):
                    line = line[1:]
            line = line.strip()
            if line == "":
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise SystemExit(
                    "train_learned_scout: line " + str(line_number) + " is not JSON: " + str(error)
                )
            if not isinstance(record, dict):
                continue
            kind = record.get("kind")
            if kind != "sample":
                continue
            where = "line " + str(line_number)
            features = record.get("features")
            if not isinstance(features, dict) or "sectors" not in features:
                raise SystemExit("train_learned_scout: " + where + " has no sectors feature")
            sectors = features["sectors"]
            if not isinstance(sectors, dict):
                raise SystemExit("train_learned_scout: " + where + " sectors is not an object")
            shape = sectors.get("shape")
            values = sectors.get("values")
            if shape != [CHANNELS, 8, 8] or not isinstance(values, list):
                raise SystemExit(
                    "train_learned_scout: " + where + " sectors shape is not [2, 8, 8]"
                )
            if len(values) != CHANNELS * CELLS:
                raise SystemExit("train_learned_scout: " + where + " sectors has not 128 values")
            decoded = []
            for index, raw in enumerate(values):
                decoded.append(whole_number(raw, where + " value " + str(index)) / UNIT)
            decision = whole_number(record.get("decision"), where + " decision")
            if decision < 0 or decision >= CELLS:
                raise SystemExit("train_learned_scout: " + where + " decision is outside 0 to 63")
            samples.append((decoded, decision))
    if not samples:
        raise SystemExit("train_learned_scout: the record file has no sample")
    return samples


class Proto:
    def __init__(self):
        self.bytes = bytearray()

    def varint(self, value):
        while value > 0x7F:
            self.bytes.append((value & 0x7F) | 0x80)
            value = value >> 7
        self.bytes.append(value)

    def key(self, field, wire):
        self.varint((field << 3) | wire)

    def vint_field(self, field, value):
        self.key(field, 0)
        self.varint(value)

    def raw_field(self, field, data):
        self.key(field, 2)
        self.varint(len(data))
        self.bytes.extend(data)

    def text(self, field, value):
        self.raw_field(field, value.encode("utf-8"))

    def message(self, field, nested):
        self.raw_field(field, nested.bytes)


def float_tensor(name, dims, values):
    tensor = Proto()
    for dim in dims:
        tensor.vint_field(1, dim)
    tensor.vint_field(2, 1)
    tensor.text(8, name)
    raw = bytearray()
    for value in values:
        raw.extend(struct.pack("<f", value))
    tensor.raw_field(9, raw)
    return tensor


def attr_int(name, value):
    attribute = Proto()
    attribute.text(1, name)
    attribute.vint_field(3, value)
    return attribute


def attr_ints(name, values):
    attribute = Proto()
    attribute.text(1, name)
    packed = Proto()
    for value in values:
        packed.varint(value)
    attribute.raw_field(8, packed.bytes)
    return attribute


def node_of(op, inputs, output, attributes):
    body = Proto()
    for name in inputs:
        body.text(1, name)
    body.text(2, output)
    body.text(4, op)
    for attribute in attributes:
        body.message(5, attribute)
    return body


def dim_value(value):
    dim = Proto()
    dim.vint_field(1, value)
    return dim


def value_info(name, elem_type, dims):
    shape = Proto()
    for dim in dims:
        shape.message(1, dim)
    tensor_type = Proto()
    tensor_type.vint_field(1, elem_type)
    tensor_type.message(2, shape)
    type_message = Proto()
    type_message.message(1, tensor_type)
    value = Proto()
    value.text(1, name)
    value.message(2, type_message)
    return value


def model_bytes(weights_1, bias_1, weights_2, bias_2):
    hidden_w = []
    for unit in range(HIDDEN):
        for channel in range(CHANNELS):
            hidden_w.append(weights_1[unit][channel])
    graph = Proto()
    graph.message(
        11, value_info("sectors", 1, [dim_value(1), dim_value(2), dim_value(8), dim_value(8)])
    )
    graph.message(5, float_tensor("hidden_w", [HIDDEN, CHANNELS, 1, 1], hidden_w))
    graph.message(5, float_tensor("hidden_b", [HIDDEN], bias_1))
    graph.message(5, float_tensor("score_w", [1, HIDDEN, 1, 1], weights_2))
    graph.message(5, float_tensor("score_b", [1], [bias_2]))
    graph.message(
        1,
        node_of(
            "Conv",
            ["sectors", "hidden_w", "hidden_b"],
            "hidden",
            [attr_ints("kernel_shape", [1, 1]), attr_ints("strides", [1, 1])],
        ),
    )
    graph.message(1, node_of("Relu", ["hidden"], "activated", []))
    graph.message(
        1,
        node_of(
            "Conv",
            ["activated", "score_w", "score_b"],
            "scores",
            [attr_ints("kernel_shape", [1, 1]), attr_ints("strides", [1, 1])],
        ),
    )
    graph.message(1, node_of("Flatten", ["scores"], "flat", [attr_int("axis", 1)]))
    graph.message(
        1,
        node_of(
            "ArgMax",
            ["flat"],
            "choice",
            [attr_int("axis", 1), attr_int("keepdims", 1), attr_int("select_last_index", 0)],
        ),
    )
    graph.message(12, value_info("choice", 7, [dim_value(1), dim_value(1)]))
    opset = Proto()
    opset.vint_field(2, 13)
    model = Proto()
    model.message(7, graph)
    model.message(8, opset)
    return bytes(model.bytes)


def agreement_line(label, correct, total):
    if total == 0:
        percent = 0.0
    else:
        percent = 100.0 * correct / total
    return f"{label}: {percent:.1f}% ({correct}/{total})"


def count_correct(samples, weights_1, bias_1, weights_2, bias_2):
    correct = 0
    total = 0
    for features, decision in samples:
        total = total + 1
        if argmax(features, weights_1, bias_1, weights_2, bias_2) == decision:
            correct = correct + 1
    return correct, total


def main(argv):
    parser = argparse.ArgumentParser(description="Train the learned scout network.")
    parser.add_argument("records", help="JSONL records from --brain-record")
    parser.add_argument("--out", required=True, help="ONNX model to write")
    args = parser.parse_args(argv)
    samples = read_records(args.records)
    weights_1, bias_1, weights_2, bias_2, correct, total, held = train(samples)
    form_w1, form_b1, form_w2, form_b2 = closed_form()
    form_w1, form_b1, form_w2, form_b2 = round_weights(form_w1, form_b1, form_w2, form_b2)
    form_correct, form_total = count_correct(held, form_w1, form_b1, form_w2, form_b2)
    payload = model_bytes(weights_1, bias_1, weights_2, bias_2)
    try:
        with open(args.out, "wb") as handle:
            handle.write(payload)
    except OSError as error:
        raise SystemExit("train_learned_scout: cannot write " + args.out + ": " + str(error))
    print(agreement_line("held-out agreement", correct, total))
    print(agreement_line("closed form agreement", form_correct, form_total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
