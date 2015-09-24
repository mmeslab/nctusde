#!/usr/bin/python

import re

if __name__ == '__main__':
	syms = open("kallsyms_emu", "r")
	trace = open("ram_write_trace_noop_ext3", "r") # change this line to open other traces

	# parse all function names from trace

	symbols_to_process = {}

	lines = trace.readlines()

	start = False

	for i in range(10, len(lines)):
		line = lines[i]

		if line[48:] == "tracing_mark_write: nctuss----1\n":
			start = True
		if line[48:] == "tracing_mark_write: nctuss----2\n":
			start = False


		if start == True:
			m = re.search('(.*) <-.*', line[48:-1])
			if m:
				symbol = m.group(1)
				symbols_to_process[symbol] = True


	# calculate total I size

	lines = syms.readlines()
	total_I_size = 0
	
	for i in range(0, len(lines)):
		line = lines[i]

		sym = line[11:-1]
		if sym in symbols_to_process:
			print sym

			address = int(line[0:8], 16)
			addressNext = int(lines[i+1][0:8], 16)
			size = addressNext - address

			total_I_size += size
			
	print total_I_size

	#print symbols_to_process
	
