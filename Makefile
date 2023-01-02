CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG
CFLAGS = -Wall -g -I..

.PHONY: all clean

all: aws

aws: aws.o ./util/http-parser/http_parser.o sock_util.o w_epoll.h

sock_util.o: sock_util.c sock_util.h

./util/http-parser/http_parser.o: ./util/http-parser/http_parser.c ./util/http-parser/http_parser.h
	make -C .. http_parser.o

clean:
	-rm -f *~
	-rm -f *.o
	-rm -f test_get_request_path
