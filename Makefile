NAG ?= ./build/labnag

all:
	@printf '%b\n' "foo\nbar\nbaz" | $(NAG) -m 'Do you really want to do this?' -l -b right : -b middle : -b left : -s Quit
