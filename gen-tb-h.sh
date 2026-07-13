#!/bin/sh

topname=$1
filename=$2

all_ports=$(${LOP_HDL} get-all-ports $topname $filename)

for i in $all_ports; do
	echo "SIG($i, $(echo $i | sed 's/\./___05F/g'))"
done > tb-sig.h

cat > tb-top.h <<EOF
#define TOP V$topname

#include "V$topname.h"
EOF

(
	printf "ports = ["
	for i in $all_ports; do
		printf "'$i', "
	done
	printf "]\n"
) > tb.py
