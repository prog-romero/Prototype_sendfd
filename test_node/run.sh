export WRK_IMAGE_PATH=/tmp/toto/toto.img
export WRK_FUNCTION="/"


 wrk  -c 2 -t 2 -d 10s -R 100 -s send.lua http://localhost:9000/



 exit 0
