# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================
import numpy as np

import mindspore.nn as nn
import mindspore.ops as ops
from mindspore import context
from mindspore import Tensor
from mindspore.ops import operations as P
from mindspore.ops import composite as C
from mindspore.common.parameter import Parameter, ParameterTuple

grad_all = C.GradOperation(get_all=True)
grad_by_list = C.GradOperation(get_by_list=True)

class CropAndResizeNet(nn.Cell):
    def __init__(self, crop_size):
        super(CropAndResizeNet, self).__init__()
        self.crop_and_resize = P.CropAndResize()
        self.crop_size = crop_size

    def construct(self, x, boxes, box_indices):
        return self.crop_and_resize(x, boxes, box_indices, self.crop_size)

    def bprop(self, x, boxes, box_indices, out, dout):
        return x, boxes, box_indices


class TestUserDefinedBpropNet(nn.Cell):
    def __init__(self, in_channel, out_channel):
        super(TestUserDefinedBpropNet, self).__init__()
        self.relu = nn.ReLU()
        self.conv = nn.Conv2d(in_channels=in_channel, out_channels=out_channel, kernel_size=2, stride=1, has_bias=False,
                              weight_init='ones', pad_mode='same')
        self.crop = CropAndResizeNet((10, 10))
        self.boxes = Tensor(np.ones((128, 4)).astype(np.float32))
        self.box_indices = Tensor(np.ones((128,)).astype(np.int32))

    def construct(self, x):
        x = self.relu(x)
        x = self.conv(x)
        x = self.crop(x, self.boxes, self.box_indices)
        return x


class TestUserDefinedBpropGradNet(nn.Cell):
    def __init__(self, net):
        super(TestUserDefinedBpropGradNet, self).__init__()
        self.net = net

    def construct(self, x):
        return grad_all(self.net)(x)


def test_user_defined_bprop():
    context.set_context(mode=context.GRAPH_MODE)
    net = TestUserDefinedBpropNet(3, 10)
    grad_net = TestUserDefinedBpropGradNet(net)
    x = Tensor(np.ones((128, 3, 12, 12)).astype(np.float32))
    grad_net(x)


class SinNet(nn.Cell):
    def __init__(self):
        super(SinNet, self).__init__()
        self.sin = ops.Sin()

    def construct(self, x):
        out = self.sin(x)
        return out


class SinGrad(nn.Cell):
    def __init__(self, network):
        super(SinGrad, self).__init__()
        self.grad = ops.GradOperation()
        self.network = network

    def construct(self, x):
        gout = self.grad(self.network)(x)
        return gout


class SinGradSec(nn.Cell):
    def __init__(self, network):
        super(SinGradSec, self).__init__()
        self.grad = ops.GradOperation()
        self.network = network

    def construct(self, x):
        gout = self.grad(self.network)(x)
        return gout


def test_second_grad_with_j_primitive():
    context.set_context(mode=context.GRAPH_MODE)
    net = SinNet()
    first_grad = SinGrad(net)
    second_grad = SinGradSec(first_grad)
    x = Tensor(np.array([1.0], dtype=np.float32))
    second_grad(x)


# A CNode being used as FV is MapMorphism after MapMorphism of call-site CNode;
def test_ad_fv_cnode_order():
    context.set_context(mode=context.GRAPH_MODE, save_graphs=True)
    class Net(nn.Cell):
        def __init__(self):
            super(Net, self).__init__()

        # cnode xay is not being MapMorphism when cnode second_level() is being MapMorphism and
        # BackPropagateFv as MapMorphism is started from output node and from left to right order.
        def construct(self, x, y):
            def first_level():
                xay = x + y

                def second_level():
                    return xay

                return second_level() + xay
            return first_level()

    input_x = Tensor(np.array([1.0], dtype=np.float32))
    input_y = Tensor(np.array([2.0], dtype=np.float32))

    net = Net()
    net.add_flags_recursive(defer_inline=True)
    grad_net = grad_all(net)
    grad_net(input_x, input_y)


# True and False branch of switch have different number of parameters.
def test_if_branch_with_different_params():
    context.set_context(mode=context.GRAPH_MODE, save_graphs=False)
    class Net(nn.Cell):
        def __init__(self):
            super(Net, self).__init__()
            self.weight1 = Parameter(Tensor(np.array([1.0], dtype=np.float32)), name="weight1")
            self.weight2 = Parameter(Tensor(np.array([2.0], dtype=np.float32)), name="weight2")

        def construct(self, idx, end, x):
            out = x
            if idx < end:
                out = out + self.weight1 * self.weight2
            else:
                out = out + self.weight1
            return out

    class GradNet(nn.Cell):
        def __init__(self, net):
            super(GradNet, self).__init__()
            self.net = net
            self.weights = ParameterTuple(net.trainable_params())

        def construct(self, idx, end, x):
            return grad_by_list(self.net, self.weights)(idx, end, x)

    idx = Tensor(np.array((0), dtype=np.int32))
    end = Tensor(np.array((3), dtype=np.int32))
    x = Tensor(np.array([2.0], dtype=np.float32))

    net = Net()
    grad_net = GradNet(net)
    grad_net(idx, end, x)


# Only lift fv in scope of lift_top_func_graph other than all func_graphs inside manager.
# Otherwise, "Illegal AnfNode for evaluating" may be reported
# because weight1 in Net may use old_parameter other than replicated one.
def test_limit_lift_fv_scope():
    context.set_context(mode=context.GRAPH_MODE, save_graphs=False)
    class Net(nn.Cell):
        def __init__(self):
            super(Net, self).__init__()
            self.weight1 = Parameter(Tensor(np.array([1.0], dtype=np.float32)), name="weight1")

        def construct(self, x, y):
            def inner_add(a, b):
                return a + b

            out = inner_add(x, y) + self.weight1
            return out

    class GradNet(nn.Cell):
        def __init__(self, net):
            super(GradNet, self).__init__()
            self.net = net
            self.weights = ParameterTuple(net.trainable_params())

        def construct(self, x, y):
            def inner_grad_add(a, b):
                return a + b

            d_weight = grad_by_list(self.net, self.weights)(x, y)[0]
            d_out = inner_grad_add(d_weight, y)
            return d_out

    x = Tensor(np.array([2.0], dtype=np.float32))
    y = Tensor(np.array([2.0], dtype=np.float32))

    net = Net()
    net.add_flags_recursive(defer_inline=True)
    grad_net = GradNet(net)
    grad_net.add_flags_recursive(defer_inline=True)
    grad_net(x, y)
