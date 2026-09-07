#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[])
{
	// check that the correct amount of arguments are given (2 in this case)
	if (argc != 3)
	{
		syslog(LOG_ERR, "Incorrect number of arguments.");
		fprintf(stderr, "Error found! Please enter 2 arguments. \n");
		return 1;
	}

	// storing the command line arguments
	const char *writefile = argv[1];
	const char *writestr = argv[2];

	// open syslog
	openlog(NULL, 0, LOG_USER);

	// log the write operation
	syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

	// opening the file to write in it
	FILE *file = fopen(writefile, "w");

	// check if file opened succesfully
	if (file == NULL)
	{
		syslog(LOG_ERR, "Could not open file %s: %s", writefile, strerror(errno));
		fprintf(stderr, "Error found! Could not create file %s\n", writefile);
		closelog();
		return 1;
	}

	// writing the string to the file
	if (fprintf(file, "%s", writestr) < 0)
	{
		syslog(LOG_ERR, "Could not write to file %s: %s", writefile, strerror(errno));
		fprintf(stderr, "Error found! Could not write to file %s\n", writefile);
		fclose(file);
		closelog();
		return 1;
	}

	// closing the file
	if (fclose(file) != 0)
	{
		syslog(LOG_ERR, "Could not close file %s: %s", writefile, strerror(errno));
		fprintf(stderr, "Error found! Could not close file %s\n", writefile);
		closelog();
		return 1;
	}

	// close syslog
	closelog();

	return 0;
}
