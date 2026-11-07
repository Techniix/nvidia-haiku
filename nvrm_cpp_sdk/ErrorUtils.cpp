#include "ErrorUtils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


NvStatusErrorCategory NvStatusErrorCategory::sInstance;


const char* NvStatusErrorCategory::name() const noexcept
{
	return "NV_STATUS";
}

std::string NvStatusErrorCategory::message(int ev) const
{
	return nvstatusToString(ev);
}


void RaiseErrno(int err)
{
	throw std::system_error(err, std::generic_category());
#if 0
	fprintf(stderr, "[!] %s(%#x)\n", strerror(err), err);
	abort();
#endif
}

void RaiseNvStatus(NV_STATUS res)
{
	// std::system_error keeps a reference to its category: it must outlive
	// the exception, so use the static instance, not a temporary.
	throw std::system_error(res, NvStatusErrorCategory::Instance());
#if 0
	fprintf(stderr, "[!] %s(%#x)\n", nvstatusToString(res), res);
	abort();
#endif
}
