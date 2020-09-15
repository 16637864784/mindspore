#!/usr/bin/env bash
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

run_ascend()
{
    if [ $2 -lt 1 ] && [ $2 -gt 8 ]
    then
        echo "error: DEVICE_NUM=$2 is not in (1-8)"
    exit 1
    fi

    if [ ! -d $5 ] && [ ! -f $5 ]
    then
        echo "error: DATASET_PATH=$5 is not a directory or file"
    exit 1
    fi

    BASEPATH=$(cd "`dirname $0`" || exit; pwd)
    export PYTHONPATH=${BASEPATH}:$PYTHONPATH
    export RANK_TABLE_FILE=$4
    if [ -d "../train" ];
    then
        rm -rf ../train
    fi
    mkdir ../train
    cd ../train || exit
    python ${BASEPATH}/../src/launch.py \
            --platform=$1 \
            --nproc_per_node=$2 \
            --visible_devices=$3 \
            --training_script=${BASEPATH}/../train.py \
            --dataset_path=$5 \
            --train_method=$6 \
            --pretrain_ckpt=$7 \
            &> ../train.log &  # dataset train folder
}

run_gpu()
{
    if [ $2 -lt 1 ] && [ $2 -gt 8 ]
    then
        echo "error: DEVICE_NUM=$2 is not in (1-8)"
    exit 1
    fi

    if [ ! -d $4 ]
    then
        echo "error: DATASET_PATH=$4 is not a directory"
    exit 1
    fi

    BASEPATH=$(cd "`dirname $0`" || exit; pwd)
    export PYTHONPATH=${BASEPATH}:$PYTHONPATH
    if [ -d "../train" ];
    then
        rm -rf ../train
    fi
    mkdir ../train
    cd ../train || exit

    export CUDA_VISIBLE_DEVICES="$3"
    mpirun -n $2 --allow-run-as-root --output-filename log_output --merge-stderr-to-stdout \
    python ${BASEPATH}/../train.py \
        --platform=$1 \
        --dataset_path=$4 \
        --train_method=$5 \
        --pretrain_ckpt=$6 \
        &> ../train.log &  # dataset train folder
}

run_cpu()
{

    if [ ! -d $2 ]
    then
        echo "error: DATASET_PATH=$2 is not a directory"
    exit 1
    fi

    BASEPATH=$(cd "`dirname $0`" || exit; pwd)
    export PYTHONPATH=${BASEPATH}:$PYTHONPATH
    if [ -d "../train" ];
    then
        rm -rf ../train
    fi
    mkdir ../train
    cd ../train || exit

    python ${BASEPATH}/../train.py \
        --platform=$1 \
        --dataset_path=$2 \
        --train_method=$3 \
        --pretrain_ckpt=$4 \
        &> ../train.log &  # dataset train folder
}

if [ $# -gt 7 ] || [ $# -lt 4 ]
then
    echo "Usage:
          Ascend: sh run_train.sh Ascend [DEVICE_NUM] [VISIABLE_DEVICES(0,1,2,3,4,5,6,7)] [RANK_TABLE_FILE] [DATASET_PATH] [TRAIN_METHOD] [CKPT_PATH]
          GPU: sh run_train.sh GPU [DEVICE_NUM] [VISIABLE_DEVICES(0,1,2,3,4,5,6,7)] [DATASET_PATH] [TRAIN_METHOD] [CKPT_PATH]
          CPU: sh run_train.sh CPU [DATASET_PATH] [TRAIN_METHOD] [CKPT_PATH]"
exit 1
fi

if [ $1 = "Ascend" ] ; then
    run_ascend "$@"
elif [ $1 = "GPU" ] ; then
    run_gpu "$@"
elif [ $1 = "CPU" ] ; then
    run_cpu "$@"
else
    echo "Unsupported device_target."
fi;
