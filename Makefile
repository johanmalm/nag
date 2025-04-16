NAG ?= ./build/labnag

all:
	@printf '%b\n' "foo\nbar\nbaz" | $(NAG) -m 'Do you really want to do this?' -l -Z right : -Z middle : -Z left : -Z foot foot -s Quit
