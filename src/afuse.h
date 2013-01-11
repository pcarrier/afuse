#ifndef __AFUSE_H
#define __AFUSE_H

// Global definitions in use through afuse source

// When closing an fd/dir, the close may fail due to a signal
// this value defines how many times we retry in this case.
// It's useful to try and close as many fd's as possible
// for the proxied fs to increase the chance an umount will
// succeed.
#define CLOSE_MAX_RETRIES 5

#endif				// __AFUSE_H
