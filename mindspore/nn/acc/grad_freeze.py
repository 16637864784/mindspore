# Copyright 2021 Huawei Technologies Co., Ltd
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
"""grad freeze"""

import numpy as np

from mindspore.nn.cell import Cell
from mindspore.nn.optim import Optimizer
from mindspore.common import Tensor, Parameter
from mindspore.common import dtype as mstype

__all__ = ['CONTINUOUS_STRATEGY', 'INTERVAL_STRATEGY',
           'split_parameters_groups', 'generate_freeze_index_sequence',
           'FreezeOpt']

CONTINUOUS_STRATEGY = 0
INTERVAL_STRATEGY = 1


def split_parameters_groups(net, freeze_para_groups_number):
    """Split parameter groups for gradients freezing training."""
    grouped_params = []
    tmp = []
    for para in net.trainable_params():
        name = para.name
        # ensure 'bn' after 'conv' is not split
        if 'bn' in name or 'bias' in name:
            tmp.append(para)
        elif len(tmp) >= 3:
            grouped_params.append(tmp)
            tmp = [para]
        else:
            tmp.append(para)
    if tmp:
        grouped_params.append(tmp)
    stride = len(grouped_params) // freeze_para_groups_number
    freeze_grouped_params = [sum(grouped_params[i * stride:], []) for i in range(freeze_para_groups_number)]
    return freeze_grouped_params


def generate_freeze_index_sequence(parameter_groups_number, freeze_strategy, freeze_p, steps_per_epoch, max_epoch):
    """Generate index sequence for gradient freezing training."""
    total_step = steps_per_epoch * max_epoch * 1.01
    # local continuous freezing training strategy, as '00001234'
    if freeze_strategy == CONTINUOUS_STRATEGY:
        zero_cnt = int(freeze_p * (parameter_groups_number - 1) / (1 - freeze_p) + 0.5)
        sub_idx = [0] * zero_cnt + list(range(1, parameter_groups_number))
        freeze_idxes = []
        while len(freeze_idxes) < total_step:
            freeze_idxes += sub_idx
        return freeze_idxes
    # interval freezing training strategy, as '01020304'
    if freeze_strategy == INTERVAL_STRATEGY:
        index_all = list(range(1, parameter_groups_number))
        prob = [x / sum(index_all) for x in index_all]
        freeze_idxes = [0]
        zero_cnt = 1
        freeze_cnt = 0
        while len(freeze_idxes) < total_step:
            freeze_p_cur = 1.0 * freeze_cnt / (zero_cnt + freeze_cnt)
            if freeze_p_cur < 1 - freeze_p:
                freeze_idxes.append(int(np.random.choice(index_all[::-1], p=prob)))
                freeze_cnt += 1
            else:
                freeze_idxes.append(0)
                zero_cnt += 1
        return freeze_idxes
    raise ValueError(f"Unsupported freezing training strategy '{freeze_strategy}'")


class FreezeOpt(Cell):
    """
    Optimizer that supports gradients freezing training.

    Args:
        opt (Optimizer): non-freezing optimizer instance, such as 'Momentum', 'SGD'.
        train_parameter_groups (Union[Tuple, List]): Groups of parameters for gradients freezing training.
        train_strategy (Union[tuple(int), list(int), Tensor]): Strategy for gradients freezing training.

    Supported Platforms:
        ``Ascend``
    """
    def __init__(self, opt, train_parameter_groups=None, train_strategy=None):
        super(FreezeOpt, self).__init__()
        if not isinstance(opt, Optimizer):
            raise TypeError(f"The first arg 'opt' must be an Optimizer instance, but got {type(opt)}")
        if train_strategy is not None and train_parameter_groups is None:
            raise ValueError("When the 'train_strategy' is specified, the value of 'train_parameter_groups' "
                             "must also be specified")
        opt_class = type(opt)
        opt_init_args = opt.init_args
        self.opts = []

        if train_parameter_groups is None:
            groups_num = 10
            step = 6
            parameters = opt.parameters
            para_groups = (parameters[(i * step):] for i in range(groups_num))
            self.opts = [opt_class(params=params, **opt_init_args) for params in para_groups]
        else:
            if not isinstance(train_parameter_groups, (tuple, list)):
                raise TypeError("The specified 'train_parameter_groups' should be tuple or list")
            for params in train_parameter_groups:
                if not isinstance(params, (tuple, list)):
                    raise TypeError("The each element of 'train_parameter_groups' should be tuple or list "
                                    "to store the Parameter")
                for para in params:
                    if not isinstance(para, Parameter):
                        raise TypeError("The element of each group should be the Parameter")

                # generate one-to-one opt corresponding to the parameter group
                self.opts.append(opt_class(params=params, **opt_init_args))

        if isinstance(train_strategy, (tuple, list)):
            for ele in train_strategy:
                if not isinstance(ele, int):
                    raise ValueError("The element in train_strategy should be int number")
            self.train_strategy = Tensor(train_strategy, mstype.int32)
        elif isinstance(train_strategy, Tensor):
            if train_strategy.ndim != 1 or train_strategy.dtype != mstype.int32:
                raise ValueError("When train_strategy is a Tensor, the dimension should be 1 and "
                                 "the dtype should be int32")
            self.train_strategy = train_strategy
        elif train_strategy is None:
            self.train_strategy = None
        else:
            raise TypeError("The specified 'train_strategy' should be None, tuple, list or Tensor")
