#!/bin/bash
# Copyright 2022 Huawei Technologies Co., Ltd
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

# Prepare and Install mindspore cpu by pip on Ubuntu 18.04.
#
# This file will:
#   - change deb source to huaweicloud mirror
#   - install mindspore dependencies via apt like gcc, libgmp
#   - install python3 & pip3 via apt and set it to default
#   - install mindspore-cpu within new installed python by pip
#
# Augments:
#   - PYTHON_VERSION: python version to install. [3.7(default), 3.8, 3.9]
#   - MINDSPORE_VERSION: mindspore version to install, required
#
# Usage:
#   Run script like `MINDSPORE_VERSION=1.6.1 bash ./ubuntu-cpu-pip.sh`.
#   To set augments, run it as `PYTHON_VERSION=3.9 MINDSPORE_VERSION=1.5.0 bash ./ubuntu-cpu-pip.sh`.

set -e

PYTHON_VERSION=${PYTHON_VERSION:-3.7}
MINDSPORE_VERSION=${MINDSPORE_VERSION:-EMPTY}

if [[ $MINDSPORE_VERSION == "EMPTY" ]]; then
    echo "MINDSPORE_VERSION not set, please check available versions at https://www.mindspore.cn/versions."
    exit 1
fi

available_py_version=(3.7 3.8 3.9)
if [[ " ${available_py_version[*]} " != *" $PYTHON_VERSION "* ]]; then
    echo "PYTHON_VERSION is '$PYTHON_VERSION', but available versions are [${available_py_version[*]}]."
    exit 1
fi

declare -A version_map=()
version_map["3.7"]="${MINDSPORE_VERSION}-cp37-cp37m"
version_map["3.8"]="${MINDSPORE_VERSION}-cp38-cp38"
version_map["3.9"]="${MINDSPORE_VERSION}-cp39-cp39"

# use huaweicloud mirror in China
sudo sed -i "s@http://.*archive.ubuntu.com@http://repo.huaweicloud.com@g" /etc/apt/sources.list
sudo sed -i "s@http://.*security.ubuntu.com@http://repo.huaweicloud.com@g" /etc/apt/sources.list
sudo apt-get update
sudo apt-get install gcc-7 libgmp-dev -y

# python
sudo add-apt-repository -y ppa:deadsnakes/ppa
sudo apt-get install python$PYTHON_VERSION python$PYTHON_VERSION-distutils python3-pip -y
sudo update-alternatives --install /usr/bin/python python /usr/bin/python$PYTHON_VERSION 100
# pip
python -m pip install -U pip -i https://pypi.tuna.tsinghua.edu.cn/simple
echo -e "alias pip='python -m pip'" >> ~/.bashrc
source ~/.bashrc
pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple

# install mindspore whl
arch=`uname -m`
pip install https://ms-release.obs.cn-north-4.myhuaweicloud.com/${MINDSPORE_VERSION}/MindSpore/cpu/${arch}/mindspore-${version_map["$PYTHON_VERSION"]}-linux_${arch}.whl --trusted-host ms-release.obs.cn-north-4.myhuaweicloud.com -i https://pypi.tuna.tsinghua.edu.cn/simple

# check mindspore installation
python -c "import mindspore;mindspore.run_check()"
