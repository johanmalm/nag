all:
	@printf '%b\n' "foo\nbar\nbaz" | ./build/nag -m 'Do you really want to do this?' -l
