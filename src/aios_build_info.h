/**************************************************************************/
/*  aios_build_info.h                                                     */
/*  Overridden at compile time by SConstruct when git is available.       */
/**************************************************************************/

#pragma once

#ifndef AIOS_GIT_COMMIT
#define AIOS_GIT_COMMIT "dev"
#endif

#ifdef AIOS_GIT_COMMIT_DATE
#define AIOS_BUILD_LABEL AIOS_GIT_COMMIT " (" AIOS_GIT_COMMIT_DATE ")"
#else
#define AIOS_BUILD_LABEL AIOS_GIT_COMMIT
#endif
