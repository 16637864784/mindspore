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
"""Summary collector callback."""

import os
import re
import json

from importlib import import_module

import numpy as np

from mindspore import log as logger
from mindspore.common.tensor import Tensor
from mindspore.common.parameter import Parameter
from mindspore.train.summary.summary_record import SummaryRecord
from mindspore.train.summary.enum import PluginEnum, ModeEnum
from mindspore.train.callback import Callback, ModelCheckpoint
from mindspore.train import lineage_pb2
from mindspore.train.callback._dataset_graph import DatasetGraph
from mindspore.nn.optim.optimizer import Optimizer
from mindspore.nn.loss.loss import _Loss
from mindspore.train._utils import check_value_type


class LineageMetadata:
    """Initialize parameters used in model lineage management."""
    train_dataset_path = 'train_dataset_path'
    valid_dataset_path = 'valid_dataset_path'
    train_network = 'train_network'
    loss_function = 'loss_function'
    loss = 'loss'
    optimizer = 'optimizer'
    learning_rate = 'learning_rate'
    epoch = 'epoch'
    step_num = 'step_num'
    parallel_mode = 'parallel_mode'
    device_num = 'device_num'
    batch_size = 'batch_size'
    model_path = 'model_path'
    model_ckpt = 'model_ckpt'
    model_size = 'model_size'
    metrics = 'metrics'
    train_dataset_size = 'train_dataset_size'
    valid_dataset_size = 'valid_dataset_size'


class SummaryCollector(Callback):
    """
    SummaryCollector can help you to collect some common information.

    It can help you to collect loss, learning late, computational graph and so on.
    SummaryCollector also persists data collected by the summary operator into a summary file.

    Note:
        1. Multiple SummaryCollector instances in callback list are not allowed.
        2. Not all information is collected at the training phase or at the eval phase.
        3. SummaryCollector always record the data collected by the summary operator.

    Args:
        summary_dir (str): The collected data will be persisted to this directory.
            If the directory does not exist, it will be created automatically.
        collect_freq (int): Set the frequency of data collection, it should be greater then zero,
            and the unit is `step`. Default: 10. If a frequency is set, we will collect data
            at (current steps % freq) == 0, and the first step will be collected at any time.
            It is important to note that if the data sink mode is used, the unit will become the `epoch`.
            It is not recommended to collect data too frequently, which can affect performance.
        collect_specified_data (Union[None, dict]): Perform custom operations on the collected data. Default: None.
            By default, if set to None, all data is collected as the default behavior.
            If you want to customize the data collected, you can do so with a dictionary.
            Examples,you can set {'collect_metric': False} to control not collecting metrics.
            The data that supports control is shown below.

            - collect_metric: Whether to collect training metrics, currently only loss is collected.
              The first output will be treated as loss, and it will be averaged.
              Optional: True/False. Default: True.
            - collect_graph: Whether to collect computational graph, currently only
              training computational graph is collected. Optional: True/False. Default: True.
            - collect_train_lineage: Whether to collect lineage data for the training phase,
              this field will be displayed on the lineage page of Mindinsight. Optional: True/False. Default: True.
            - collect_eval_lineage: Whether to collect lineage data for the eval phase,
              this field will be displayed on the lineage page of Mindinsight. Optional: True/False. Default: True.
            - collect_input_data: Whether to collect dataset for each training. Currently only image data is supported.
              Optional: True/False. Default: True.
            - collect_dataset_graph: Whether to collect dataset graph for the training phase.
              Optional: True/False. Default: True.
            - histogram_regular: Collect weight and bias for parameter distribution page display in MindInsight.
              This field allows regular strings to control which parameters to collect.
              Default: None, it means only the first five parameters are collected.
              It is not recommended to collect too many parameters at once, as it can affect performance.
              Note that if you collect too many parameters and run out of memory, the training will fail.
        keep_default_action (bool): This field affects the collection behavior of the 'collect_specified_data' field.
            Optional: True/False, Default: True.
            True: means that after specified data is set, non-specified data is collected as the default behavior.
            False: means that after specified data is set, only the specified data is collected,
            and the others are not collected.
        custom_lineage_data (Union[dict, None]): Allows you to customize the data and present it on the MingInsight
            lineage page. In the custom data, the key type support str, and the value type support str/int/float.
            Default: None, it means there is no custom data.
        collect_tensor_freq (Optional[int]): Same as the `collect_freq`, but controls TensorSummary specifically.
            Default: None, which means the frequency is auto-calculated just to collect at most 50 steps TensorSummary.
        max_file_size (Optional[int]): The maximum size in bytes each file can be written to the disk.
            Default: None, which means no limit.

    Raises:
        ValueError: If the parameter value is not expected.
        TypeError: If the parameter type is not expected.
        RuntimeError: If an error occurs during data collection.

    Examples:
        >>> # Simple usage:
        >>> summary_collector = SummaryCollector(summary_dir='./summary_dir')
        >>> model.train(epoch, dataset, callbacks=summary_collector)
        >>>
        >>> # Do not collect metric and collect the first layer parameter, others are collected by default
        >>> specified={'collect_metric': False, 'histogram_regular': '^conv1.*'}
        >>> summary_collector = SummaryCollector(summary_dir='./summary_dir', collect_specified_data=specified)
        >>> model.train(epoch, dataset, callbacks=summary_collector)
        >>>
        >>> # Only collect metric, custom lineage data and record data that collected by the summary operator,
        >>> # others are not collected
        >>> specified = {'collect_metric': True}
        >>> summary_collector = SummaryCollector('./summary_dir',
        >>>                                      collect_specified_data=specified,
        >>>                                      keep_default_action=False,
        >>>                                      custom_lineage_data={'version': 'resnet50_v1'}
        >>>                                      )
        >>> model.train(epoch, dataset, callbacks=summary_collector)
    """

    _DEFAULT_SPECIFIED_DATA = {
        'collect_metric': True,
        'collect_graph': True,
        'collect_train_lineage': True,
        'collect_eval_lineage': True,
        'collect_input_data': True,
        'collect_dataset_graph': True,
        'histogram_regular': None
    }

    def __init__(self,
                 summary_dir,
                 collect_freq=10,
                 collect_specified_data=None,
                 keep_default_action=True,
                 custom_lineage_data=None,
                 collect_tensor_freq=None,
                 max_file_size=None):
        super(SummaryCollector, self).__init__()

        self._summary_dir = self._process_summary_dir(summary_dir)
        self._record = None

        self._check_positive('collect_freq', collect_freq)
        self._collect_freq = collect_freq

        self._check_positive('collect_tensor_freq', collect_tensor_freq, allow_none=True)
        self._collect_tensor_freq = collect_tensor_freq

        self._check_positive('max_file_size', max_file_size, allow_none=True)
        self._max_file_size = max_file_size

        self._check_action(keep_default_action)

        self._collect_specified_data = self._process_specified_data(collect_specified_data, keep_default_action)
        msg = f"For 'collect_specified_data' the value after processing is: {self._collect_specified_data}."
        logger.info(msg)

        self._check_custom_lineage_data(custom_lineage_data)
        self._custom_lineage_data = custom_lineage_data

        self._temp_optimizer = None
        self._has_saved_custom_data = False
        self._is_parse_loss_success = True
        self._first_step = True
        self._dataset_sink_mode = True

    def __enter__(self):
        self._record = SummaryRecord(log_dir=self._summary_dir, max_file_size=self._max_file_size)
        self._first_step, self._dataset_sink_mode = True, True
        return self

    def __exit__(self, *err):
        self._record.close()

    @staticmethod
    def _process_summary_dir(summary_dir):
        """Check the summary dir, and create a new directory if it not exists."""
        check_value_type('summary_dir', summary_dir, str)
        summary_dir = summary_dir.strip()
        if not summary_dir:
            raise ValueError('For `summary_dir` the value should be a valid string of path, but got empty string.')

        summary_dir = os.path.realpath(summary_dir)
        if not os.path.exists(summary_dir):
            os.makedirs(summary_dir, exist_ok=True)
        else:
            if not os.path.isdir(summary_dir):
                raise NotADirectoryError('For `summary_dir` it should be a directory path.')

        return summary_dir

    @staticmethod
    def _check_positive(name, value, allow_none=False):
        """Check if the value to be int type and positive."""
        if allow_none:
            return
        check_value_type(name, value, int)
        if value <= 0:
            raise ValueError(f'For `{name}` the value should be greater than 0, but got `{value}`.')

    @staticmethod
    def _check_custom_lineage_data(custom_lineage_data):
        """
        Check user custom lineage data.

        Args:
            custom_lineage_data (dict): The user custom defined data.

        Raises:
            TypeError: If the type of parameters is invalid.
        """
        if custom_lineage_data is None:
            return

        check_value_type('custom_lineage_data', custom_lineage_data, [dict, type(None)])
        for key, value in custom_lineage_data.items():
            check_value_type(f'custom_lineage_data -> {key}', key, str)
            check_value_type(f'the value of custom_lineage_data -> {key}', value, (int, str, float))

    @staticmethod
    def _check_action(action):
        """Check action type."""
        check_value_type('keep_default_action', action, bool)

    def _process_specified_data(self, specified_data, action):
        """Check specified data type and value."""
        if specified_data is None:
            if action:
                return self._DEFAULT_SPECIFIED_DATA
            return dict()

        check_value_type('collect_specified_data', specified_data, [dict, type(None)])

        for param_name in specified_data:
            check_value_type(param_name, param_name, [str])

        unexpected_params = set(specified_data) - set(self._DEFAULT_SPECIFIED_DATA)
        if unexpected_params:
            raise ValueError(f'For `collect_specified_data` the keys {unexpected_params} are unsupported, '
                             f'expect the follow keys: {list(self._DEFAULT_SPECIFIED_DATA.keys())}')

        if 'histogram_regular' in specified_data:
            check_value_type('histogram_regular', specified_data.get('histogram_regular'), (str, type(None)))

        bool_items = set(self._DEFAULT_SPECIFIED_DATA) - {'histogram_regular'}
        for item in bool_items:
            if item in specified_data:
                check_value_type(item, specified_data.get(item), bool)

        if action:
            result = dict(self._DEFAULT_SPECIFIED_DATA)
            result.update(specified_data)
        else:
            result = specified_data
        return result

    def begin(self, run_context):
        cb_params = run_context.original_args()
        self._check_callbacks(cb_params)

        if cb_params.mode not in ModeEnum.to_list():
            raise ValueError('Only support `train` (model.train) and `eval` (model.eval) mode, '
                             'but got `{cb_params.mode}` mode.')

        self._record.set_mode(cb_params.mode)
        if cb_params.mode == ModeEnum.TRAIN.value:
            # Note: if model.init is not executed then the computed graph will not be obtained here
            # The purpose of recording the graph here was to collect_freq if it was set to a large size,
            # but also want to see the graph as soon after compilation.
            self._collect_graphs(cb_params)

            self._collect_dataset_graph(cb_params)
            if self._collect_tensor_freq is None:
                total_step = cb_params.epoch_num * cb_params.batch_num
                self._collect_tensor_freq = max(self._collect_freq, total_step // 50)

        if self._custom_lineage_data and not self._has_saved_custom_data:
            packaged_custom_data = self._package_custom_lineage_data(self._custom_lineage_data)
            self._record.add_value('custom_lineage_data', 'custom_lineage_data', packaged_custom_data)
            self._has_saved_custom_data = True

        # There's nothing special about setting step to 0 here, just to satisfy the interface call
        self._record.record(step=0)

    def step_end(self, run_context):
        cb_params = run_context.original_args()
        if cb_params.mode != ModeEnum.TRAIN.value:
            return
        if self._first_step:
            # Notice: This way of determining whether dataset sink mode is True does not work in the eval scenario
            self._dataset_sink_mode = cb_params.cur_step_num == cb_params.batch_num
            self._collect_at_step_end(cb_params, plugin_filter=None)
            self._first_step = False
        else:
            current = cb_params.cur_epoch_num if self._dataset_sink_mode else cb_params.cur_step_num
            if current % self._collect_freq == 0 and current % self._collect_tensor_freq == 0:
                self._collect_at_step_end(cb_params, plugin_filter=None)
            elif current % self._collect_tensor_freq == 0:
                self._collect_at_step_end(cb_params, lambda plugin: plugin == PluginEnum.TENSOR.value)
            elif current % self._collect_freq == 0:
                self._collect_at_step_end(cb_params, lambda plugin: plugin != PluginEnum.TENSOR.value)


    def _collect_at_step_end(self, cb_params, plugin_filter):
        self._collect_input_data(cb_params)
        self._collect_metric(cb_params)
        self._collect_histogram(cb_params)
        self._record.record(cb_params.cur_step_num, plugin_filter=plugin_filter)


    def end(self, run_context):
        cb_params = run_context.original_args()
        if cb_params.mode == ModeEnum.TRAIN.value:
            self._collect_train_lineage(cb_params)
        else:
            self._collect_eval_lineage(cb_params)

        # This is a workaround to avoid record '_summary_tensor_cache'.
        self._record.set_mode('eval')
        # There's nothing special about setting step to 0 here, just to satisfy the interface call
        self._record.record(step=0)

    def _check_callbacks(self, cb_params):
        """Check there if there are duplicate instances of SummaryCollector."""
        callbacks = cb_params.list_callback

        is_find = False
        for callback in callbacks:
            if type(callback).__name__ == self.__class__.__name__:
                if not is_find:
                    is_find = True
                    continue
                raise ValueError(f"There are more than one {self.__class__.__name__} instance in callback list,"
                                 f"but expected only one {self.__class__.__name__} instance.")

    @staticmethod
    def _package_custom_lineage_data(custom_lineage_data):
        """
        Package user-defined lineage data into binary data.

        Args:
            custom_lineage_data (dict): User custom lineage data.

        Returns:
            UserDefinedInfo, a object of lineage_pb2.UserDefinedInfo.
        """
        user_defined_info = lineage_pb2.UserDefinedInfo()
        for key, value in custom_lineage_data.items():
            if isinstance(value, int):
                attr_name = "map_int32"
            elif isinstance(value, float):
                attr_name = "map_double"
            else:
                attr_name = "map_str"

            user_info = user_defined_info.user_info.add()
            getattr(user_info, attr_name)[key] = value

        return user_defined_info

    def _collect_input_data(self, cb_params):
        """Only support to collect image data."""
        if not self._collect_specified_data.get('collect_input_data'):
            return

        input_data = getattr(cb_params, 'train_dataset_element', None)
        if input_data is None:
            self._collect_specified_data['collect_input_data'] = False
            logger.info("The 'train_dataset_element' in cb_params is None, maybe there is dataset sink mode.")
            return

        if isinstance(input_data, (list, tuple)):
            input_data = input_data[0]
        try:
            self._record.add_value(PluginEnum.IMAGE.value, 'input_data/auto', input_data)
        except ValueError:
            logger.warning('The input data of network are not image, so will not collect by SummaryCollector.')
            self._collect_specified_data['collect_input_data'] = False
            return

    def _collect_dataset_graph(self, cb_params):
        """Only collect train dataset graph."""
        if not self._collect_specified_data.get('collect_dataset_graph'):
            return

        # After analysis, we think that the validated dataset graph and the training dataset graph
        # should be consistent under normal scenarios, so only the training dataset graph is collected.
        if cb_params.mode == ModeEnum.TRAIN.value:
            train_dataset = cb_params.train_dataset
            dataset_graph = DatasetGraph()
            graph_bytes = dataset_graph.package_dataset_graph(train_dataset)
            self._record.add_value('dataset_graph', 'train_dataset', graph_bytes)

    def _collect_graphs(self, cb_params):
        """Collect the graph of train network and eval network."""
        if not self._collect_specified_data.get('collect_graph'):
            return

        network = cb_params.train_network if cb_params.mode == ModeEnum.TRAIN.value else cb_params.eval_network
        graph_proto = network.get_func_graph_proto()
        if graph_proto is None:
            return

        self._record.add_value(PluginEnum.GRAPH.value, 'train_network/auto', graph_proto)

    def _collect_metric(self, cb_params):
        """Collect metric, currently only collection Loss is supported."""
        if not self._collect_specified_data.get('collect_metric'):
            return

        loss = self._get_loss(cb_params)
        if loss is None:
            return

        try:
            self._record.add_value(PluginEnum.SCALAR.value, 'loss/auto', loss)
        except ValueError:
            logger.warning("The output of network is not a scalar, so will not collect loss in SummaryCollector.")
            self._collect_specified_data['collect_metric'] = False

    def _get_loss(self, cb_params):
        """
        Get loss from the network output.

        Args:
            cb_params (_InternalCallbackParam): Callback parameters.

        Returns:
            Union[Tensor, None], if parse loss success, will return a Tensor value(shape is [1]), else return None.
        """
        if not self._is_parse_loss_success:
            # If parsing has failed before, avoid repeating it
            return None

        output = cb_params.net_outputs
        if output is None:
            logger.warning("Can not find any output by this network, so will not collect loss in SummaryCollector.")
            self._is_parse_loss_success = False
            return None

        if isinstance(output, (int, float, Tensor)):
            loss = output
        elif isinstance(output, (list, tuple)) and output:
            # If the output is a list, since the default network returns loss first,
            # we assume that the first one is loss.
            loss = output[0]
        else:
            logger.warning("The output type could not be identified, so no loss was recorded in SummaryCollector.")
            self._is_parse_loss_success = False
            return None

        if not isinstance(loss, Tensor):
            loss = Tensor(loss)

        precision = 4
        loss = Tensor(round(np.mean(loss.asnumpy()), precision))
        return loss

    def _get_optimizer(self, cb_params):
        """
        Get optimizer from the cb_params or parse from the network.

        Args:
            cb_params (_InternalCallbackParam): Callback parameters.

        Returns:
            Union[Optimizer, None], if parse optimizer success, will return a optimizer, else return None.
        """
        # 'optimizer_failed' means find optimizer failed, so we will not collect data about optimizer.
        optimizer_failed = 'Failed'
        if self._temp_optimizer == optimizer_failed:
            return None

        if self._temp_optimizer is not None:
            return self._temp_optimizer

        optimizer = cb_params.optimizer
        if optimizer is None:
            network = cb_params.train_network if cb_params.mode == 'train' else cb_params.eval_network
            optimizer = self._parse_optimizer_by_network(network)

        if optimizer is None or not isinstance(optimizer, Optimizer):
            logger.warning("Can not find optimizer in network, or the optimizer does not inherit MindSpore's "
                           "optimizer, so we will not collect data about optimizer in SummaryCollector.")
            optimizer = None

        self._temp_optimizer = optimizer if optimizer is not None else optimizer_failed

        return optimizer

    @staticmethod
    def _parse_optimizer_by_network(network):
        """Parse optimizer from network, if parse success will return a optimizer, else return None."""
        optimizer = None
        for _, cell in network.cells_and_names():
            if isinstance(cell, Optimizer):
                return cell
            try:
                optimizer = getattr(cell, 'optimizer')
            except AttributeError:
                continue

            if not isinstance(optimizer, Optimizer):
                continue

            # Optimizer found successfully
            break

        return optimizer

    def _collect_histogram(self, cb_params):
        """Collect histogram data, contain the parameter weight and bias."""
        # Note: if there is not a key named `histogram_regular` in `self._collect_specified_data`,
        # it means we will not collect histogram data.
        if 'histogram_regular' not in self._collect_specified_data:
            return

        optimizer = self._get_optimizer(cb_params)
        if optimizer is None:
            return

        parameters = optimizer.parameters
        regular = self._collect_specified_data.get('histogram_regular')
        if regular is not None:
            for parameter in parameters:
                if re.match(regular, parameter.name):
                    self._record.add_value(PluginEnum.HISTOGRAM.value, parameter.name+'/auto', parameter.data)
            return

        # Note: If `histogram_regular` in `self._collect_specified_data` and the value is None,
        # we will collect the first five parameters.
        default_parameter_count = 5
        for parameter in parameters[:default_parameter_count]:
            self._record.add_value(PluginEnum.HISTOGRAM.value, parameter.name+'/auto', parameter.data)

    @staticmethod
    def _get_learning_rate(optimizer):
        """
        parse the learning rate from optimizer.

        Args:
            optimizer (Optimizer): A optimizer which inherit the MindSpore Optimizer class.

        Returns:
            Union[Tensor, None], if parse learning rate success, will return a Tensor, else return None.
        """
        learning_rate = optimizer.learning_rate
        if not isinstance(learning_rate, Parameter):
            logger.info("The learning rate detected in the optimizer is not a Parameter type, so it is not recorded.")
            return None
        return learning_rate.data

    def _collect_train_lineage(self, cb_params):
        """Collect train lineage data, the detail refer to lineage_pb2.TrainLineage."""
        if not self._collect_specified_data.get('collect_train_lineage'):
            return
        train_lineage = {}
        loss = self._get_loss(cb_params)
        if loss:
            loss_numpy = loss.asnumpy()
            loss = float(np.atleast_1d(loss_numpy)[0])
            train_lineage[LineageMetadata.loss] = loss
        else:
            train_lineage[LineageMetadata.loss] = None

        optimizer = self._get_optimizer(cb_params)
        learning_rate = self._get_learning_rate(optimizer) if optimizer is not None else None

        if learning_rate is not None:
            train_lineage[LineageMetadata.learning_rate] = list(np.atleast_1d(learning_rate.asnumpy()))[0]
        else:
            train_lineage[LineageMetadata.learning_rate] = None
        train_lineage[LineageMetadata.optimizer] = type(optimizer).__name__ if optimizer else None
        train_lineage[LineageMetadata.train_network] = type(cb_params.network).__name__

        loss_fn = self._get_loss_fn(cb_params)
        train_lineage[LineageMetadata.loss_function] = type(loss_fn).__name__ if loss_fn else None

        train_lineage[LineageMetadata.epoch] = cb_params.epoch_num
        train_lineage[LineageMetadata.step_num] = cb_params.cur_step_num
        train_lineage[LineageMetadata.parallel_mode] = cb_params.parallel_mode
        train_lineage[LineageMetadata.device_num] = cb_params.device_number

        ckpt_file_path = self._get_ckpt_file_path(cb_params)
        train_lineage[LineageMetadata.model_path] = json.dumps(dict(ckpt=ckpt_file_path))

        model_size = os.path.getsize(ckpt_file_path) if ckpt_file_path else 0
        train_lineage[LineageMetadata.model_size] = model_size

        self._parse_dataset(cb_params, train_lineage)

        train_lineage_message = self._package_train_lineage_message(train_lineage)

        self._record.add_value(PluginEnum.TRAIN_LINEAGE.value, 'train_lineage', train_lineage_message)

    @staticmethod
    def _package_train_lineage_message(train_lineage):
        """
        Package train lineage data into binary data.

        Args:
            train_lineage (dict): The train lineage dict, refer to the attribute of `_collect_train_lineage` method.

        Returns:
            TrainLineage, a object of lineage_pb2.TrainLineage.
        """
        lineage_message = lineage_pb2.TrainLineage()

        if train_lineage.get(LineageMetadata.train_network) is not None:
            lineage_message.algorithm.network = train_lineage.get(LineageMetadata.train_network)
        if train_lineage.get(LineageMetadata.loss) is not None:
            lineage_message.algorithm.loss = train_lineage.get(LineageMetadata.loss)

        # Construct train_dataset message.
        if train_lineage.get(LineageMetadata.train_dataset_path) is not None:
            lineage_message.train_dataset.train_dataset_path = train_lineage.get(LineageMetadata.train_dataset_path)
        if train_lineage.get(LineageMetadata.train_dataset_size) is not None:
            lineage_message.train_dataset.train_dataset_size = train_lineage.get(LineageMetadata.train_dataset_size)

        # Construct model message
        lineage_message.model.path = train_lineage.get(LineageMetadata.model_path)
        lineage_message.model.size = train_lineage.get(LineageMetadata.model_size)

        # Construct hyper_parameters message.
        if train_lineage.get(LineageMetadata.learning_rate) is not None:
            lineage_message.hyper_parameters.learning_rate = train_lineage.get(LineageMetadata.learning_rate)
        if train_lineage.get(LineageMetadata.optimizer) is not None:
            lineage_message.hyper_parameters.optimizer = train_lineage.get(LineageMetadata.optimizer)
        if train_lineage.get(LineageMetadata.loss_function) is not None:
            lineage_message.hyper_parameters.loss_function = train_lineage.get(LineageMetadata.loss_function)
        if train_lineage.get(LineageMetadata.parallel_mode) is not None:
            lineage_message.hyper_parameters.parallel_mode = train_lineage.get(LineageMetadata.parallel_mode)

        lineage_message.hyper_parameters.epoch = train_lineage.get(LineageMetadata.epoch)
        lineage_message.hyper_parameters.device_num = train_lineage.get(LineageMetadata.device_num)
        lineage_message.hyper_parameters.batch_size = train_lineage.get(LineageMetadata.batch_size)

        return lineage_message

    def _parse_dataset(self, cb_params, lineage_dict):
        """
        Analyze Dataset to get the dataset path and dataset size.

        Args:
            cb_params (_InternalCallbackParam): Callback parameters.
            lineage_dict (dict): The lineage dict, refer to the attribute
                of `_collect_train_lineage` method or `_collect_eval_lineage`.

        Returns:
            dict, the lineage metadata.
        """
        dataset = cb_params.train_dataset if cb_params.mode == ModeEnum.TRAIN.value else cb_params.valid_dataset

        try:
            dataset_path = self._get_dataset_path(dataset)
        except IndexError:
            dataset_path = None

        if dataset_path and os.path.isfile(dataset_path):
            dataset_dir = os.path.dirname(dataset_path)
        else:
            dataset_dir = dataset_path

        batch_num = dataset.get_dataset_size()
        batch_size = dataset.get_batch_size()
        dataset_size = int(batch_num * batch_size)

        lineage_dict[LineageMetadata.batch_size] = batch_size

        if cb_params.mode == ModeEnum.TRAIN.value:
            lineage_dict[LineageMetadata.train_dataset_path] = dataset_dir
            lineage_dict[LineageMetadata.train_dataset_size] = dataset_size
        else:
            lineage_dict[LineageMetadata.valid_dataset_path] = dataset_dir
            lineage_dict[LineageMetadata.valid_dataset_size] = dataset_size

        return lineage_dict

    def _get_dataset_path(self, output_dataset):
        """
        Get dataset path of MindDataset object.

        Args:
            output_dataset (Union[Dataset, ImageFolderDatasetV2, MnistDataset, Cifar10Dataset, Cifar100Dataset,
                VOCDataset, CelebADataset, MindDataset, ManifestDataset, TFRecordDataset, TextFileDataset]):
                Refer to mindspore.dataset.Dataset.

        Returns:
            str, dataset path.

        Raises:
            IndexError: it means get dataset path failed.
        """
        dataset_package = import_module('mindspore.dataset')
        dataset_dir_set = (dataset_package.ImageFolderDatasetV2, dataset_package.MnistDataset,
                           dataset_package.Cifar10Dataset, dataset_package.Cifar100Dataset,
                           dataset_package.VOCDataset, dataset_package.CelebADataset)
        dataset_file_set = (dataset_package.MindDataset, dataset_package.ManifestDataset)
        dataset_files_set = (dataset_package.TFRecordDataset, dataset_package.TextFileDataset)

        if isinstance(output_dataset, dataset_file_set):
            return output_dataset.dataset_file
        if isinstance(output_dataset, dataset_dir_set):
            return output_dataset.dataset_dir
        if isinstance(output_dataset, dataset_files_set):
            return output_dataset.dataset_files[0]
        return self._get_dataset_path(output_dataset.children[0])

    @staticmethod
    def _get_ckpt_file_path(cb_params):
        """
        Get checkpoint file path from MindSpore callback list.

        Args:
            cb_params (_InternalCallbackParam): Callback parameters.

        Returns:
            Union[str, None], if parse success will checkpoint file absolute path, else return None.
        """
        callbacks = cb_params.list_callback
        ckpt_file_path = None
        for callback in callbacks:
            if isinstance(callback, ModelCheckpoint):
                ckpt_file_path = callback.latest_ckpt_file_name

        if ckpt_file_path:
            ckpt_file_path = os.path.realpath(ckpt_file_path)

        return ckpt_file_path

    @staticmethod
    def _get_loss_fn(cb_params):
        """
        Get loss function by cb_params and analyzing network.

        Args:
            cb_params (_InternalCallbackParam): Callback parameters.

        Returns:
            Union[Cell, None], a Cell object, if parse failed, will return None.
        """
        loss_fn = cb_params.loss_fn
        if loss_fn is not None:
            return loss_fn

        if cb_params.mode == ModeEnum.TRAIN.value:
            network = cb_params.train_network
        else:
            network = cb_params.eval_network

        for _, cell in network.cells_and_names():
            if isinstance(cell, _Loss):
                loss_fn = cell
                break
        return loss_fn

    def _collect_eval_lineage(self, cb_params):
        """Collect eval lineage data, the detail refer to lineage_pb2.EvaluationLineage."""
        if not self._collect_specified_data.get('collect_eval_lineage'):
            return
        eval_lineage = dict()

        eval_lineage[LineageMetadata.metrics] = json.dumps(cb_params.metrics)
        self._parse_dataset(cb_params, eval_lineage)

        eval_lineage_message = self._package_eval_lineage_message(eval_lineage)
        self._record.add_value(PluginEnum.EVAL_LINEAGE.value, 'eval_lineage', eval_lineage_message)

    @staticmethod
    def _package_eval_lineage_message(eval_lineage):
        """
        Package eval lineage data into binary data.

        Args:
            eval_lineage (dict): The eval lineage dict, refer to the attribute of `_collect_eval_lineage` method.

        Returns:
            EvaluationLineage, a object of lineage_pb2.EvaluationLineage.
        """
        lineage_message = lineage_pb2.EvaluationLineage()

        if eval_lineage.get(LineageMetadata.metrics) is not None:
            lineage_message.metric = eval_lineage.get(LineageMetadata.metrics)
        if eval_lineage.get(LineageMetadata.valid_dataset_path) is not None:
            lineage_message.valid_dataset.valid_dataset_path = eval_lineage.get(LineageMetadata.valid_dataset_path)
        if eval_lineage.get(LineageMetadata.valid_dataset_size) is not None:
            lineage_message.valid_dataset.valid_dataset_size = eval_lineage.get(LineageMetadata.valid_dataset_size)

        return lineage_message
