#!/bin/sh

# check for missing parameters
if [ $# -ne 2 ]
then
	echo "Error found! There is a missing parameter."
	exit 1
fi

writefile=$1
writestr=$2

# create the directory where the file will be located
mkdir -p "$(dirname "$writefile")"

# write the string to the file or overwrite the file contents
echo "$writestr" > "$writefile"

# check if the file was not created
if [ $? -ne 0 ]
then
	echo "Error found! The file could not be created"
	exit 1
fi
