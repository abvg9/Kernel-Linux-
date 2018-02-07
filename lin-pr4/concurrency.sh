#!/bin/bash
MODNAME="my_mod"

if [[ $1 =~ ^[0-9]+$ ]] ;then
	n_times="$1"
else
	n_times=10
fi

for ((i=0;i<$n_times;i++)) ; do
	if (( RANDOM % 2 ))
		then echo add $(($RANDOM % 20)) > /proc/$MODNAME
		else cat /proc/$MODNAME
	fi
done
