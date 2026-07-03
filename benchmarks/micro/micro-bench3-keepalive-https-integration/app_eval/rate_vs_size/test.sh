export WRK_IMAGE_PATH=/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/app_eval/base_latence/images/img-1024kb.jpg


export WRK_PATH=/function/objectrecognition

/home/tchiaze/wrk2/wrk -t4 -c4 -d10s -R3 --timeout 100s --latency -s /home/tchiaze/Master2_ACS_SUPAERO_ISAE/Stage/Prototype_sendfd/benchmarks/micro/micro-bench3-keepalive-https-integration/app_eval/rate_vs_size/client/post_image.lua http://192.168.2.2:8080/function/objectrecognition


exit 0
