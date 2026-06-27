#!/bin/sh

topname=$1
filename=$2

for i in $(${LOP_HDL} get-all-ports $topname $filename); do
	echo "SIG($i, $i)"
done > tb-sig.h

for i in $(${LOP_HDL} get-in-ports $topname $filename); do
	echo "COND($i, $i)"
done > tb-cond.h

cat > tb-top.h <<EOF
#define TOP V$topname

#include "V$topname.h"
EOF
