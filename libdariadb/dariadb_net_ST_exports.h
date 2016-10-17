
#ifndef DARIADBNET_ST_EXPORTS_H
#define DARIADBNET_ST_EXPORTS_H

#ifdef SHARED_EXPORTS_BUILT_AS_STATIC
#  define DARIADBNET_ST_EXPORTS
#  define LIBDARIADB_NO_EXPORT
#else
#  ifndef DARIADBNET_ST_EXPORTS
#    ifdef libdariadb_EXPORTS
        /* We are building this library */
#      define DARIADBNET_ST_EXPORTS 
#    else
        /* We are using this library */
#      define DARIADBNET_ST_EXPORTS 
#    endif
#  endif

#  ifndef LIBDARIADB_NO_EXPORT
#    define LIBDARIADB_NO_EXPORT 
#  endif
#endif

#ifndef LIBDARIADB_DEPRECATED
#  define LIBDARIADB_DEPRECATED __declspec(deprecated)
#endif

#ifndef LIBDARIADB_DEPRECATED_EXPORT
#  define LIBDARIADB_DEPRECATED_EXPORT DARIADBNET_ST_EXPORTS LIBDARIADB_DEPRECATED
#endif

#ifndef LIBDARIADB_DEPRECATED_NO_EXPORT
#  define LIBDARIADB_DEPRECATED_NO_EXPORT LIBDARIADB_NO_EXPORT LIBDARIADB_DEPRECATED
#endif

#define DEFINE_NO_DEPRECATED 0
#if DEFINE_NO_DEPRECATED
# define LIBDARIADB_NO_DEPRECATED
#endif

#endif
