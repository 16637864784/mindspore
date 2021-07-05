#!/bin/bash
source ./scripts/base_functions.sh

# Run converter on x86 platform:
function Run_Converter() {
    # Unzip x86 runtime and converter
    cd ${x86_path} || exit 1
    tar -zxf mindspore-lite-${version}-linux-x64.tar.gz || exit 1
    cd ${x86_path}/mindspore-lite-${version}-linux-x64/ || exit 1

    cp tools/converter/converter/converter_lite ./ || exit 1
    export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:./tools/converter/lib/:./tools/converter/third_party/glog/lib

    rm -rf ${ms_models_path}
    mkdir -p ${ms_models_path}

    # Prepare the config file list
    local npu_cfg_file_list=("$models_npu_config")
    # Convert models:
    # $1:cfgFileList; $2:inModelPath; $3:outModelPath; $4:logFile; $5:resultFile;
    Convert "${npu_cfg_file_list[*]}" $models_path $ms_models_path $run_converter_log_file $run_converter_result_file
}

# Run on npu platform:
function Run_npu() {
    # Push files to the phone
    Push_Files $arm64_path "aarch64" $version $benchmark_test_path "adb_push_log.txt" $device_id
    # Prepare the config file list
    local npu_cfg_file_list=("$models_npu_config")
    # Run converted models:
    # $1:cfgFileList; $2:modelPath; $3:dataPath; $4:logFile; $5:resultFile; $6:platform; $7:processor; $8:phoneId;
    Run_Benchmark "${npu_cfg_file_list[*]}" . '/data/local/tmp' $run_npu_log_file $run_benchmark_result_file 'arm64' 'NPU' $device_id
}

# Run on npu and fp16 platform:
function Run_npu_fp16() {
    # Push files to the phone
    Push_Files $arm64_path "aarch64" $version $benchmark_test_path "adb_push_log.txt" $device_id
    # Prepare the config file list
    local npu_fp16_cfg_file_list=("$models_npu_fp16_config")
    # Run converted models:
    # $1:cfgFileList; $2:modelPath; $3:dataPath; $4:logFile; $5:resultFile; $6:platform; $7:processor; $8:phoneId;
    Run_Benchmark "${npu_fp16_cfg_file_list[*]}" . '/data/local/tmp' $run_npu_fp16_log_file $run_benchmark_result_file 'arm64' 'NPU' $device_id
}

basepath=$(pwd)
echo ${basepath}
#set -e

# Example:sh run_benchmark_npu.sh -r /home/temp_test -m /home/temp_test/models -d "8KE5T19620002408" -e arm_cpu
while getopts "r:m:d:e:" opt; do
    case ${opt} in
        r)
            release_path=${OPTARG}
            echo "release_path is ${OPTARG}"
            ;;
        m)
            models_path=${OPTARG}
            echo "models_path is ${OPTARG}"
            ;;
        d)
            device_id=${OPTARG}
            echo "device_id is ${OPTARG}"
            ;;
        e)
            backend=${OPTARG}
            echo "backend is ${OPTARG}"
            ;;
        ?)
        echo "unknown para"
        exit 1;;
    esac
done

# mkdir train

x86_path=${release_path}/ubuntu_x86
file_name=$(ls ${x86_path}/*linux-x64.tar.gz)
IFS="-" read -r -a file_name_array <<< "$file_name"
version=${file_name_array[2]}

# Set models config filepath
models_npu_config=${basepath}/../config/models_npu.cfg
models_npu_fp16_config=${basepath}/../config/models_npu_fp16.cfg

ms_models_path=${basepath}/ms_models

# Write converter result to temp file
run_converter_log_file=${basepath}/run_converter_log.txt
echo ' ' > ${run_converter_log_file}

run_converter_result_file=${basepath}/run_converter_result.txt
echo ' ' > ${run_converter_result_file}

# Run converter
echo "start Run converter ..."
Run_Converter
Run_converter_status=$?
sleep 1

# Check converter result and return value
if [[ ${Run_converter_status} = 0 ]];then
    echo "Run converter success"
    Print_Converter_Result $run_converter_result_file
else
    echo "Run converter failed"
    cat ${run_converter_log_file}
    Print_Converter_Result $run_converter_result_file
    exit 1
fi

# Write benchmark result to temp file
run_benchmark_result_file=${basepath}/run_benchmark_result.txt
echo ' ' > ${run_benchmark_result_file}

run_npu_log_file=${basepath}/run_npu_log.txt
echo 'run npu logs: ' > ${run_npu_log_file}

run_npu_fp16_log_file=${basepath}/run_npu_fp16_log.txt
echo 'run npu fp16 logs: ' > ${run_npu_fp16_log_file}

# Copy the MindSpore models:
echo "Push files to the arm and run benchmark"
benchmark_test_path=${basepath}/benchmark_test
rm -rf ${benchmark_test_path}
mkdir -p ${benchmark_test_path}
cp -a ${ms_models_path}/*.ms ${benchmark_test_path} || exit 1

backend=${backend:-"all"}
isFailed=0

if [[ $backend == "all" || $backend == "npu" ]]; then
    # Run on npu
    arm64_path=${release_path}/android_aarch64
    # mv ${arm64_path}/*train-android-aarch64* ./train
    file_name=$(ls ${arm64_path}/*android-aarch64.tar.gz)
    IFS="-" read -r -a file_name_array <<< "$file_name"
    version=${file_name_array[2]}

    echo "start Run npu ..."
    Run_npu
    Run_npu_status=$?
    sleep 1
    if [[ ${Run_npu_status} != 0 ]];then
        echo "Run_npu failed"
        cat ${run_npu_log_file}
        isFailed=1
    fi

    echo "start Run npu fp16 ..."
    Run_npu_fp16
    Run_npu_fp16_status=$?
    sleep 1
    if [[ ${Run_npu_fp16_status} != 0 ]];then
        echo "Run_npu_fp16 failed"
        cat ${run_npu_fp16_log_file}
        isFailed=1
    fi
fi

echo "Run_npu and Run_npu_fp16 ended"
Print_Benchmark_Result $run_benchmark_result_file
exit ${isFailed}
