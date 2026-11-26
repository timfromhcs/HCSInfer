"""Writes two tiny SYNTHETIC ONNX models (a single Resize node, nearest-neighbour x4) used only to smoke-test the
dynamic ONNX Runtime backend end to end. They are not super-resolution models. Needs the `onnx` package.

    python gen_nn_onnx.py <output_dir>
"""
import sys
import os
import onnx
from onnx import helper, TensorProto


def build(path, fixed):
    h = w = 32 if fixed else None
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, h, w])
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3, None if not fixed else h * 4, None if not fixed else w * 4])
    scales = helper.make_tensor("scales", TensorProto.FLOAT, [4], [1.0, 1.0, 4.0, 4.0])
    node = helper.make_node(
        "Resize", ["input", "", "scales"], ["output"], mode="nearest",
        coordinate_transformation_mode="asymmetric", nearest_mode="floor")
    graph = helper.make_graph([node], "nn4x", [x], [y], initializer=[scales])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
build(os.path.join(out, "nn4x_dynamic.onnx"), False)
build(os.path.join(out, "nn4x_fixed32.onnx"), True)
print("wrote", out)
