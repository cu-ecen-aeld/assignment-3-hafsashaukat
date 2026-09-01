#!/bin/sh

# checks that both parameters are specified
if [ $# -ne 2 ]
then
	echo "Error found! There is a missing parameter."
	exit 1
fi

filesdir=$1
searchstr=$2

# checks that filesdir is a directory that exists
if [ ! -d "$filesdir" ]
then
	echo "Error found! $filesdir is not a directory that exists."
	exit 1
fi

# count all the files in filesdir
files_num=$(find "$filesdir" -type f | wc -l)

# counts all the matching lines with searchstr in files dir
matching_num=$(grep -r "$searchstr" "$filesdir" | wc -l)

echo "The number of files are $files_num and the number of matching lines are $matching_num"
