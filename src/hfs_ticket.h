#ifndef HFS_TICKET_H
#define HFS_TICKET_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

#include "hfs_inode_runtime.h"

/* Operations used by the internal POSIX range-ticket scheduler. */
typedef enum HfsPosixTicketOperation {
	HFS_POSIX_TICKET_READ = 0,
	HFS_POSIX_TICKET_WRITE,
	HFS_POSIX_TICKET_FENCE
} HfsPosixTicketOperation;

/* One synchronous POSIX request retained while its range dependencies drain. */
struct hfs_posix_io_ticket {
	HfsInodeRuntime *runtime;
	HfsFile *file;
	HfsPosixIoTicket *next;
	pthread_mutex_t waitLock;
	pthread_cond_t waitCondition;
	uint64_t ticketId;
	uint64_t submitSeq;
	uint64_t offset;
	uint64_t length;
	uint64_t parentId;
	uint32_t pendingDependencies;
	HfsPosixTicketOperation operation;
	int result;
	bool ready;
	bool done;
	bool admitted;
	bool dependencyWait;
};

/* Enable the ticket scheduler unless the operator explicitly disables it. */
bool hfsPosixTicketEnabled(void);

/* Initialize one stack-owned ticket before admission. */
int hfsPosixTicketInit(HfsPosixIoTicket *ticket, HfsInodeRuntime *runtime,
			      HfsFile *file, uint64_t offset, uint64_t length,
			      HfsPosixTicketOperation operation, uint64_t parentId);

/* Insert a ticket and compute its bounded range dependencies. */
int hfsPosixTicketAdmit(HfsPosixIoTicket *ticket);

/* Wait until all earlier conflicting tickets have completed. */
int hfsPosixTicketWaitReady(HfsPosixIoTicket *ticket);

/* Retire a ticket, promote dependents, and publish the request result. */
void hfsPosixTicketComplete(HfsPosixIoTicket *ticket, int result);

/* Destroy the ticket wait primitives after completion and removal. */
void hfsPosixTicketDestroy(HfsPosixIoTicket *ticket);

#endif /* HFS_TICKET_H */
