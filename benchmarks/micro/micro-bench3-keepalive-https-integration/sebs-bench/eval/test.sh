export  WRK_BODY_FILE=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/inputs/dynamic-html.json
   export  WRK_PATH=/function/dynamic-html
   export  WRK_PERF_FILE=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/eval/results/dynamic-html/.perf_dynamic-html_4.tmp
   /home/tchiaze/wrk2/wrk -t1 -c1 -d20s -R4 --timeout 60s  -s /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/sebs-bench/eval/client/post_json.lua https://192.168.2.2:8443/function/dynamic-html
