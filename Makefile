CPPFLAGS = #-DDEBUG -DLOG_LEVEL=LOG_DEBUG
CFLAGS = -Wall -g -I..

.PHONY: all clean

build: aws

aws: aws.o ./util/http-parser/http_parser.o sock_util.o w_epoll.h
	gcc $(CPPFLAG) $(CFLAGS) aws.o sock_util.o ./util/http-parser/http_parser.o -o aws -laio

sock_util.o: sock_util.c sock_util.h

./util/http-parser/http_parser.o: ./util/http-parser/http_parser.c ./util/http-parser/http_parser.h
	make -C ./util/http-parser/

clean:
	-rm -f *~
	-rm -f *.o
	-rm -f test_get_request_path
	-rm aws
