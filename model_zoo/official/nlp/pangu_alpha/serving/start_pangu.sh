#!/bin/bash

unset http_proxy
unset https_proxy
export GLOG_v=1

start_serving_server()
{
  echo "### start serving server, see serving_server.log for detail ###"
  python3 serving_server.py > serving_server.log 2>&1 &
  if [ $? -ne 0 ]
  then
    echo "serving server failed to start."
  fi

  result=`grep -E 'Begin waiting ready of all agents' serving_server.log | wc -l`
  count=0
  while [[ ${result} -ne 1 && ${count} -lt 100 ]]
  do
    sleep 1

    num=`ps -ef | grep 'serving_server.py' | grep -v grep | wc -l`
    if [ $num -eq 0 ]
    then
      echo "start serving server failed, see log serving_server.log for more detail" && exit 1
    fi

    count=$(($count+1))
    result=`grep -E 'Begin waiting ready of all agents' serving_server.log | wc -l`
  done

  if [ ${count} -eq 100 ]
  then
    echo "start serving server failed, see log serving_server.log for more detail" && exit 1
  fi
  echo "### start serving server end ###"
}

start_serving_agent()
{
  echo "### start serving agent, see serving_agent.log for detail ###"
  python3 serving_agent.py > serving_agent.log 2>&1 &
  if [ $? -ne 0 ]
  then
    echo "serving agent failed to start."
  fi

  result=`grep -E 'Child 0: Receive success' serving_agent.log | wc -l`
  count=0
  while [[ ${result} -ne 1 && ${count} -lt 1800 ]]
  do
    sleep 1
    num=`ps -ef | grep 'serving_agent.py' | grep -v grep | wc -l`
    if [ $num -eq 0 ]
    then
      bash stop_pangu.sh
      echo "start serving agent failed, see log serving_agent.log for more detail" && exit 1
    fi

    count=$(($count+1))
    result=`grep -E 'Child 0: Receive success' serving_agent.log | wc -l`
  done

  if [ ${count} -eq 1800 ]
  then
    bash stop_pangu.sh
    echo "start serving agent failed, see log serving_agent.log  for more detail" && exit 1
  fi
  echo "### start serving agent end ###"
}

start_flask()
{
  echo "### start flask server, see flask.log for detail ###"
  python3 flask/client.py > flask.log 2>&1 &
  if [ $? -ne 0 ]
  then
    echo "flask server failed to start."
  fi

  result=`grep -E 'Press CTRL\+C to quit' flask.log | wc -l`
  count=0
  while [[ ${result} -ne 1 && ${count} -lt 10 ]]
  do
    sleep 1

    num=`ps -ef | grep 'flask/client.py' | grep -v grep | wc -l`
    if [ $num -eq 0 ]
    then
      bash stop_pangu.sh
      echo "start flask server failed, see log flask.log for more detail" && exit 1
    fi

    count=$(($count+1))
    result=`grep -E 'Press CTRL\+C to quit' flask.log | wc -l`
  done

  if [ ${count} -eq 10 ]
  then
    bash stop_pangu.sh
    echo "start flask server failed, see log flask.log for more detail" && exit 1
  fi
  echo "### start flask server end ###"
  cat flask.log
}

bash stop_pangu.sh
rm -f serving_server.log serving_agent.log flask.log
start_serving_server
start_serving_agent
start_flask
