The fallback that names a session directory when `/dev/urandom` cannot
be read no longer mixes an uninitialised `struct timespec` into the name
when `clock_gettime` fails; it uses `time(NULL)` instead.
