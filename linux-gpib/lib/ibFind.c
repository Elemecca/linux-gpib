
#include "ib_internal.h"
#include <ibP.h>
#include <string.h>
#include <stdlib.h>

int ibfind(char *dev)
{
	int index;
	char *envptr;
	int retval;
	int uDesc;
	ibConf_t *conf;
	int status;

	/* load config */

	envptr = getenv("IB_CONFIG");
	if(envptr)
		retval = ibParseConfigFile(envptr);
	else
		retval = ibParseConfigFile(DEFAULT_CONFIG_FILE);
	if(retval < 0)
	{
		setIberr( EDVR );
		setIbsta( ERR );
		return -1;
	}

	if((index = ibFindDevIndex(dev)) < 0)
	{ /* find desired entry */
		setIberr( EDVR );
		setIbsta( ERR );
		return -1;
	}

	conf = &ibFindConfigs[ index ];

	uDesc = my_ibdev( conf->board, conf->pad, conf->sad, conf->usec_timeout,
		conf->send_eoi, conf->eos, conf->eos_flags );
	if(uDesc < 0)
	{
		fprintf(stderr, "ibfind failed to get descriptor\n");
		return -1;
	}
	conf = general_enter_library( uDesc, 1, 0 );

	if(conf->flags & CN_SDCL)
	{
		status = ibclr(uDesc);
		if( status & ERR ) return -1;
	}

	if(strcmp(conf->init_string, ""))
	{
		status = ibwrt(uDesc, conf->init_string, strlen(conf->init_string));
		if( status & ERR )
			return -1;
	}

	return uDesc;
}











