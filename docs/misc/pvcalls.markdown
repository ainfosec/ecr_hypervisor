# PV Calls Protocol version 1

## Glossary

The following is a list of terms and definitions used in the Xen
community. If you are a Xen contributor you can skip this section.

* PV

  Short for paravirtualized.

* Dom0

  First virtual machine that boots. In most configurations Dom0 is
  privileged and has control over hardware devices, such as network
  cards, graphic cards, etc.

* DomU

  Regular unprivileged Xen virtual machine.

* Domain

  A Xen virtual machine. Dom0 and all DomUs are all separate Xen
  domains.

* Guest

  Same as domain: a Xen virtual machine.

* Frontend

  Each DomU has one or more paravirtualized frontend drivers to access
  disks, network, console, graphics, etc. The presence of PV devices is
  advertized on XenStore, a cross domain key-value database. Frontends
  are similar in intent to the virtio drivers in Linux.

* Backend

  A Xen paravirtualized backend typically runs in Dom0 and it is used to
  export disks, network, console, graphics, etcs, to DomUs. Backends can
  live both in kernel space and in userspace. For example xen-blkback
  lives under drivers/block in the Linux kernel and xen_disk lives under
  hw/block in QEMU. Paravirtualized backends are similar in intent to
  virtio device emulators.

* VMX and SVM
  
  On Intel processors, VMX is the CPU flag for VT-x, hardware
  virtualization support. It corresponds to SVM on AMD processors.



## Rationale

PV Calls is a paravirtualized protocol that allows the implementation of
a set of POSIX functions in a different domain. The PV Calls frontend
sends POSIX function calls to the backend, which implements them and
returns a value to the frontend and acts on the function call.

This version of the document covers networking function calls, such as
connect, accept, bind, release, listen, poll, recvmsg and sendmsg; but
the protocol is meant to be easily extended to cover different sets of
calls. Unimplemented commands return ENOTSUP.

PV Calls provide the following benefits:
* full visibility of the guest behavior on the backend domain, allowing
  for inexpensive filtering and manipulation of any guest calls
* excellent performance

Specifically, PV Calls for networking offer these advantages:
* guest networking works out of the box with VPNs, wireless networks and
  any other complex configurations on the host
* guest services listen on ports bound directly to the backend domain IP
  addresses
* localhost becomes a secure host wide network for inter-VMs
  communications


## Design

### Why Xen?

PV Calls are part of an effort to create a secure runtime environment
for containers (Open Containers Initiative images to be precise). PV
Calls are based on Xen, although porting them to other hypervisors is
possible. Xen was chosen because of its security and isolation
properties and because it supports PV guests, a type of virtual machines
that does not require hardware virtualization extensions (VMX on Intel
processors and SVM on AMD processors). This is important because PV
Calls is meant for containers and containers are often run on top of
public cloud instances, which do not support nested VMX (or SVM) as of
today (early 2017). Xen PV guests are lightweight, minimalist, and do
not require machine emulation: all properties that make them a good fit
for this project.

### Xenstore

The frontend and the backend connect via [xenstore] to
exchange information. The toolstack creates front and back nodes with
state of [XenbusStateInitialising]. The protocol node name
is **pvcalls**.  There can only be one PV Calls frontend per domain.

#### Frontend XenBus Nodes

version
     Values:         <string>

     Protocol version, chosen among the ones supported by the backend
     (see **versions** under [Backend XenBus Nodes]). Currently the
     value must be "1".

port
     Values:         <uint32_t>

     The identifier of the Xen event channel used to signal activity
     in the command ring.

ring-ref
     Values:         <uint32_t>

     The Xen grant reference granting permission for the backend to map
     the sole page in a single page sized command ring.

#### Backend XenBus Nodes

versions
     Values:         <string>

     List of comma separated protocol versions supported by the backend.
     For example "1,2,3". Currently the value is just "1", as there is
     only one version.

max-page-order
     Values:         <uint32_t>

     The maximum supported size of a memory allocation in units of
     log2n(machine pages), e.g. 1 = 2 pages, 2 == 4 pages, etc. It must
     be 1 or more.

function-calls
     Values:         <uint32_t>

     Value "0" means that no calls are supported.
     Value "1" means that socket, connect, release, bind, listen, accept
     and poll are supported.

#### State Machine

Initialization:

    *Front*                               *Back*
    XenbusStateInitialising               XenbusStateInitialising
    - Query virtual device                - Query backend device
      properties.                           identification data.
    - Setup OS device instance.           - Publish backend features
    - Allocate and initialize the           and transport parameters
      request ring.                                      |
    - Publish transport parameters                       |
      that will be in effect during                      V
      this connection.                            XenbusStateInitWait
                 |
                 |
                 V
       XenbusStateInitialised

                                          - Query frontend transport parameters.
                                          - Connect to the request ring and
                                            event channel.
                                                         |
                                                         |
                                                         V
                                                 XenbusStateConnected

     - Query backend device properties.
     - Finalize OS virtual device
       instance.
                 |
                 |
                 V
        XenbusStateConnected

Once frontend and backend are connected, they have a shared page, which
will is used to exchange messages over a ring, and an event channel,
which is used to send notifications.

Shutdown:

    *Front*                            *Back*
    XenbusStateConnected               XenbusStateConnected
                |
                |
                V
       XenbusStateClosing

                                       - Unmap grants
                                       - Unbind event channels
                                                 |
                                                 |
                                                 V
                                         XenbusStateClosing

    - Unbind event channels
    - Free rings
    - Free data structures
               |
               |
               V
       XenbusStateClosed

                                       - Free remaining data structures
                                                 |
                                                 |
                                                 V
                                         XenbusStateClosed


### Commands Ring

The shared ring is used by the frontend to forward POSIX function calls
to the backend. We shall refer to this ring as **commands ring** to
distinguish it from other rings which can be created later in the
lifecycle of the protocol (see [Indexes Page and Data ring]). The grant
reference for shared page for this ring is shared on xenstore (see
[Frontend XenBus Nodes]). The ring format is defined using the familiar
`DEFINE_RING_TYPES` macro (`xen/include/public/io/ring.h`).  Frontend
requests are allocated on the ring using the `RING_GET_REQUEST` macro.
The list of commands below is in calling order.

The format is defined as follows:
    
    #define PVCALLS_SOCKET         0
    #define PVCALLS_CONNECT        1
    #define PVCALLS_RELEASE        2
    #define PVCALLS_BIND           3
    #define PVCALLS_LISTEN         4
    #define PVCALLS_ACCEPT         5
    #define PVCALLS_POLL           6

    struct xen_pvcalls_request {
    	uint32_t req_id; /* private to guest, echoed in response */
    	uint32_t cmd;    /* command to execute */
    	union {
    		struct xen_pvcalls_socket {
    			uint64_t id;
    			uint32_t domain;
    			uint32_t type;
    			uint32_t protocol;
    			#ifdef CONFIG_X86_32
    			uint8_t pad[4];
    			#endif
    		} socket;
    		struct xen_pvcalls_connect {
    			uint64_t id;
    			uint8_t addr[28];
    			uint32_t len;
    			uint32_t flags;
    			grant_ref_t ref;
    			uint32_t evtchn;
    			#ifdef CONFIG_X86_32
    			uint8_t pad[4];
    			#endif
    		} connect;
    		struct xen_pvcalls_release {
    			uint64_t id;
    			uint8_t reuse;
    			#ifdef CONFIG_X86_32
    			uint8_t pad[7];
    			#endif
    		} release;
    		struct xen_pvcalls_bind {
    			uint64_t id;
    			uint8_t addr[28];
    			uint32_t len;
    		} bind;
    		struct xen_pvcalls_listen {
    			uint64_t id;
    			uint32_t backlog;
    			#ifdef CONFIG_X86_32
    			uint8_t pad[4];
    			#endif
    		} listen;
    		struct xen_pvcalls_accept {
    			uint64_t id;
    			uint64_t id_new;
    			grant_ref_t ref;
    			uint32_t evtchn;
    		} accept;
    		struct xen_pvcalls_poll {
    			uint64_t id;
    		} poll;
    		/* dummy member to force sizeof(struct xen_pvcalls_request) to match across archs */
    		struct xen_pvcalls_dummy {
    			uint8_t dummy[56];
    		} dummy;
    	} u;
    };

The first two fields are common for every command. Their binary layout
is:

    0       4       8
    +-------+-------+
    |req_id |  cmd  |
    +-------+-------+

- **req_id** is generated by the frontend and is a cookie used to
  identify one specific request/response pair of commands. Not to be
  confused with any command **id** which are used to identify a socket
  across multiple commands, see [Socket].
- **cmd** is the command requested by the frontend:

    - `PVCALLS_SOCKET`:  0
    - `PVCALLS_CONNECT`: 1
    - `PVCALLS_RELEASE`: 2
    - `PVCALLS_BIND`:    3
    - `PVCALLS_LISTEN`:  4
    - `PVCALLS_ACCEPT`:  5
    - `PVCALLS_POLL`:    6

Both fields are echoed back by the backend. See [Socket families and
address format] for the format of the **addr** field of connect and
bind. The maximum size of command specific arguments is 56 bytes. Any
future command that requires more than that will need a bump the
**version** of the protocol.

Similarly to other Xen ring based protocols, after writing a request to
the ring, the frontend calls `RING_PUSH_REQUESTS_AND_CHECK_NOTIFY` and
issues an event channel notification when a notification is required.

Backend responses are allocated on the ring using the `RING_GET_RESPONSE` macro.
The format is the following:

    struct xen_pvcalls_response {
        uint32_t req_id;
        uint32_t cmd;
        int32_t ret;
        uint32_t pad;
        union {
    		struct _xen_pvcalls_socket {
    			uint64_t id;
    		} socket;
    		struct _xen_pvcalls_connect {
    			uint64_t id;
    		} connect;
    		struct _xen_pvcalls_release {
    			uint64_t id;
    		} release;
    		struct _xen_pvcalls_bind {
    			uint64_t id;
    		} bind;
    		struct _xen_pvcalls_listen {
    			uint64_t id;
    		} listen;
    		struct _xen_pvcalls_accept {
    			uint64_t id;
    		} accept;
    		struct _xen_pvcalls_poll {
    			uint64_t id;
    		} poll;
    		struct _xen_pvcalls_dummy {
    			uint8_t dummy[8];
    		} dummy;
    	} u;
    };

The first four fields are common for every response. Their binary layout
is:

    0       4       8       12      16
    +-------+-------+-------+-------+
    |req_id |  cmd  |  ret  |  pad  |
    +-------+-------+-------+-------+

- **req_id**: echoed back from request
- **cmd**: echoed back from request
- **ret**: return value, identifies success (0) or failure (see [Error
  numbers] in further sections). If the **cmd** is not supported by the
  backend, ret is ENOTSUP.
- **pad**: padding

After calling `RING_PUSH_RESPONSES_AND_CHECK_NOTIFY`, the backend checks whether
it needs to notify the frontend and does so via event channel.

A description of each command, their additional request and response
fields follow.


#### Socket

The **socket** operation corresponds to the POSIX [socket][socket]
function. It creates a new socket of the specified family, type and
protocol. **id** is freely chosen by the frontend and references this
specific socket from this point forward. See [Socket families and
address format] to see which ones are supported by different versions of
the protocol.

Request fields:

- **cmd** value: 0
- additional fields:
  - **id**: generated by the frontend, it identifies the new socket
  - **domain**: the communication domain
  - **type**: the socket type
  - **protocol**: the particular protocol to be used with the socket, usually 0

Request binary layout:

    8       12      16      20     24       28
    +-------+-------+-------+-------+-------+
    |       id      |domain | type  |protoco|
    +-------+-------+-------+-------+-------+

Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16       20       24
    +-------+--------+
    |       id       |
    +-------+--------+

Return value:

  - 0 on success
  - See the [POSIX socket function][connect] for error names; see
    [Error numbers] in further sections.

#### Connect

The **connect** operation corresponds to the POSIX [connect][connect]
function. It connects a previously created socket (identified by **id**)
to the specified address.

The connect operation creates a new shared ring, which we'll call **data
ring**. The data ring is used to send and receive data from the
socket. The connect operation passes two additional parameters:
**evtchn** and **ref**. **evtchn** is the port number of a new event
channel which will be used for notifications of activity on the data
ring. **ref** is the grant reference of the **indexes page**: a page
which contains shared indexes that point to the write and read locations
in the **data ring**. The **indexes page** also contains the full array
of grant references for the **data ring**. When the frontend issues a
**connect** command, the backend:

- finds its own internal socket corresponding to **id**
- connects the socket to **addr**
- maps the grant reference **ref**, the indexes page, see struct
  pvcalls_data_intf
- maps all the grant references listed in `struct pvcalls_data_intf` and
  uses them as shared memory for the **data ring**
- bind the **evtchn**
- replies to the frontend

The [Indexes Page and Data ring] format will be described in the
following section. The **data ring** is unmapped and freed upon issuing
a **release** command on the active socket identified by **id**. A
frontend state change can also cause data rings to be unmapped.

Request fields:

- **cmd** value: 0
- additional fields:
  - **id**: identifies the socket
  - **addr**: address to connect to, see [Socket families and address format]
  - **len**: address length up to 28 octets
  - **flags**: flags for the connection, reserved for future usage
  - **ref**: grant reference of the indexes page
  - **evtchn**: port number of the evtchn to signal activity on the **data ring**

Request binary layout:

    8       12      16      20      24      28      32      36      40      44
    +-------+-------+-------+-------+-------+-------+-------+-------+-------+
    |       id      |                            addr                       |
    +-------+-------+-------+-------+-------+-------+-------+-------+-------+
    | len   | flags |  ref  |evtchn |
    +-------+-------+-------+-------+

Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16      20      24
    +-------+-------+
    |       id      |
    +-------+-------+

Return value:

  - 0 on success
  - See the [POSIX connect function][connect] for error names; see
    [Error numbers] in further sections.

#### Release

The **release** operation closes an existing active or a passive socket.

When a release command is issued on a passive socket, the backend
releases it and frees its internal mappings. When a release command is
issued for an active socket, the data ring and indexes page are also
unmapped and freed:

- frontend sends release command for an active socket
- backend releases the socket
- backend unmaps the data ring
- backend unmaps the indexes page
- backend unbinds the event channel
- backend replies to frontend with an **ret** value
- frontend frees data ring, indexes page and unbinds event channel

Request fields:

- **cmd** value: 1
- additional fields:
  - **id**: identifies the socket
  - **reuse**: an optimization hint for the backend. The field is
    ignored for passive sockets. When set to 1, the frontend lets the
    backend know that it will reuse exactly the same set of grant pages
    (indexes page and data ring) and event channel when creating one of
    the next active sockets. The backend can take advantage of it by
    delaying unmapping grants and unbinding the event channel. The
    backend is free to ignore the hint. Reused data rings are found by
    **ref**, the grant reference of the page containing the indexes.

Request binary layout:

    8       12      16    17
    +-------+-------+-----+
    |       id      |reuse|
    +-------+-------+-----+

Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16      20      24
    +-------+-------+
    |       id      |
    +-------+-------+

Return value:

  - 0 on success
  - See the [POSIX shutdown function][shutdown] for error names; see
    [Error numbers] in further sections.

#### Bind

The **bind** operation corresponds to the POSIX [bind][bind] function.
It assigns the address passed as parameter to a previously created
socket, identified by **id**. **Bind**, **listen** and **accept** are
the three operations required to have fully working passive sockets and
should be issued in that order.

Request fields:

- **cmd** value: 2
- additional fields:
  - **id**: identifies the socket
  - **addr**: address to connect to, see [Socket families and address
    format]
  - **len**: address length up to 28 octets

Request binary layout:

    8       12      16      20      24      28      32      36      40      44
    +-------+-------+-------+-------+-------+-------+-------+-------+-------+
    |       id      |                            addr                       |
    +-------+-------+-------+-------+-------+-------+-------+-------+-------+
    |  len  |
    +-------+

Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16      20      24
    +-------+-------+
    |       id      |
    +-------+-------+

Return value:

  - 0 on success
  - See the [POSIX bind function][bind] for error names; see
    [Error numbers] in further sections.


#### Listen

The **listen** operation marks the socket as a passive socket. It corresponds to
the [POSIX listen function][listen].

Reuqest fields:

- **cmd** value: 3
- additional fields:
  - **id**: identifies the socket
  - **backlog**: the maximum length to which the queue of pending
    connections may grow in number of elements

Request binary layout:

    8       12      16      20
    +-------+-------+-------+
    |       id      |backlog|
    +-------+-------+-------+

Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16      20      24
    +-------+-------+
    |       id      |
    +-------+-------+

Return value:
  - 0 on success
  - See the [POSIX listen function][listen] for error names; see
    [Error numbers] in further sections.


#### Accept

The **accept** operation extracts the first connection request on the
queue of pending connections for the listening socket identified by
**id** and creates a new connected socket. The id of the new socket is
also chosen by the frontend and passed as an additional field of the
accept request struct (**id_new**). See the [POSIX accept function][accept]
as reference.

Similarly to the **connect** operation, **accept** creates new [Indexes
Page and Data ring]. The **data ring** is used to send and receive data from
the socket. The **accept** operation passes two additional parameters:
**evtchn** and **ref**. **evtchn** is the port number of a new event
channel which will be used for notifications of activity on the data
ring. **ref** is the grant reference of the **indexes page**: a page
which contains shared indexes that point to the write and read locations
in the **data ring**. The **indexes page** also contains the full array of
grant references for the **data ring**.

The backend will reply to the request only when a new connection is
successfully accepted, i.e. the backend does not return EAGAIN or
EWOULDBLOCK.

Example workflow:

- frontend issues an **accept** request
- backend waits for a connection to be available on the socket
- a new connection becomes available
- backend accepts the new connection
- backend creates an internal mapping from **id_new** to the new socket
- backend maps the grant reference **ref**, the indexes page, see struct
  pvcalls_data_intf
- backend maps all the grant references listed in `struct
  pvcalls_data_intf` and uses them as shared memory for the new data
  ring **in** and **out** arrays
- backend binds to the **evtchn**
- backend replies to the frontend with a **ret** value

Request fields:

- **cmd** value: 4
- additional fields:
  - **id**: id of listening socket
  - **id_new**: id of the new socket
  - **ref**: grant reference of the indexes page
  - **evtchn**: port number of the evtchn to signal activity on the data ring

Request binary layout:

    8       12      16      20      24      28      32
    +-------+-------+-------+-------+-------+-------+
    |       id      |    id_new     |  ref  |evtchn |
    +-------+-------+-------+-------+-------+-------+

Response additional fields:

- **id**: id of the listening socket, echoed back from request

Response binary layout:

    16      20      24
    +-------+-------+
    |       id      |
    +-------+-------+

Return value:

  - 0 on success
  - See the [POSIX accept function][accept] for error names; see
    [Error numbers] in further sections.


#### Poll

In this version of the protocol, the **poll** operation is only valid
for passive sockets. For active sockets, the frontend should look at the
indexes on the **indexes page**. When a new connection is available in
the queue of the passive socket, the backend generates a response and
notifies the frontend.

Request fields:

- **cmd** value: 5
- additional fields:
  - **id**: identifies the listening socket

Request binary layout:

    8       12      16
    +-------+-------+
    |       id      |
    +-------+-------+


Response additional fields:

- **id**: echoed back from request

Response binary layout:

    16       20       24
    +--------+--------+
    |        id       |
    +--------+--------+

Return value:

  - 0 on success
  - See the [POSIX poll function][poll] for error names; see
    [Error numbers] in further sections.

#### Expanding the protocol

It is possible to introduce new commands without changing the protocol
ABI. Naturally, a feature flag among the backend xenstore nodes should
advertise the availability of a new set of commands.

If a new command requires parameters in struct xen_pvcalls_request
larger than 56 bytes, which is the current size of the request, then the
protocol version should be increased. One way to implement the large
request structure without disrupting the current ABI is to introduce a
new command, such as PVCALLS_CONNECT_EXTENDED, and a flag to specify
that the request uses two request slots, for a total of 112 bytes.

#### Error numbers

The numbers corresponding to the error names specified by POSIX are:

    [EPERM]         -1
    [ENOENT]        -2
    [ESRCH]         -3
    [EINTR]         -4
    [EIO]           -5
    [ENXIO]         -6
    [E2BIG]         -7
    [ENOEXEC]       -8
    [EBADF]         -9
    [ECHILD]        -10
    [EAGAIN]        -11
    [EWOULDBLOCK]   -11
    [ENOMEM]        -12
    [EACCES]        -13
    [EFAULT]        -14
    [EBUSY]         -16
    [EEXIST]        -17
    [EXDEV]         -18
    [ENODEV]        -19
    [EISDIR]        -21
    [EINVAL]        -22
    [ENFILE]        -23
    [EMFILE]        -24
    [ENOSPC]        -28
    [EROFS]         -30
    [EMLINK]        -31
    [EDOM]          -33
    [ERANGE]        -34
    [EDEADLK]       -35
    [EDEADLOCK]     -35
    [ENAMETOOLONG]  -36
    [ENOLCK]        -37
    [ENOTEMPTY]     -39
    [ENOSYS]        -38
    [ENODATA]       -61
    [ETIME]         -62
    [EBADMSG]       -74
    [EOVERFLOW]     -75
    [EILSEQ]        -84
    [ERESTART]      -85
    [ENOTSOCK]      -88
    [EOPNOTSUPP]    -95
    [EAFNOSUPPORT]  -97
    [EADDRINUSE]    -98
    [EADDRNOTAVAIL] -99
    [ENOBUFS]       -105
    [EISCONN]       -106
    [ENOTCONN]      -107
    [ETIMEDOUT]     -110
    [ENOTSUP]      -524

#### Socket families and address format

The following definitions and explicit sizes, together with POSIX
[sys/socket.h][address] and [netinet/in.h][in] define socket families and
address format. Please be aware that only the **domain** `AF_INET`, **type**
`SOCK_STREAM` and **protocol** `0` are supported by this version of the
specification, others return ENOTSUP.

    #define AF_UNSPEC   0
    #define AF_UNIX     1   /* Unix domain sockets      */
    #define AF_LOCAL    1   /* POSIX name for AF_UNIX   */
    #define AF_INET     2   /* Internet IP Protocol     */
    #define AF_INET6    10  /* IP version 6         */

    #define SOCK_STREAM 1
    #define SOCK_DGRAM  2
    #define SOCK_RAW    3

    /* generic address format */
    struct sockaddr {
        uint16_t sa_family_t;
        char sa_data[26];
    };

    struct in_addr {
        uint32_t s_addr;
    };

    /* AF_INET address format */
    struct sockaddr_in {
        uint16_t         sa_family_t;
        uint16_t         sin_port;
        struct in_addr   sin_addr;
        char             sin_zero[20];
    };


### Indexes Page and Data ring

Data rings are used for sending and receiving data over a connected socket. They
are created upon a successful **accept** or **connect** command.
The **sendmsg** and **recvmsg** calls are implemented by sending data and
receiving data from a data ring, and updating the corresponding indexes
on the **indexes page**.

Firstly, the **indexes page** is shared by a **connect** or **accept**
command, see **ref** parameter in their sections. The content of the
**indexes page** is represented by `struct pvcalls_ring_intf`, see
below. The structure contains the list of grant references which
constitute the **in** and **out** buffers of the data ring, see ref[]
below. The backend maps the grant references contiguously. Of the
resulting shared memory, the first half is dedicated to the **in** array
and the second half to the **out** array. They are used as circular
buffers for transferring data, and, together, they are the data ring.


  +---------------------------+                 Indexes page
  | Command ring:             |                 +----------------------+
  | @0: xen_pvcalls_connect:  |                 |@0 pvcalls_data_intf: |
  | @44: ref  +-------------------------------->+@76: ring_order = 1   |
  |                           |                 |@80: ref[0]+          |
  +---------------------------+                 |@84: ref[1]+          |
                                                |           |          |
                                                |           |          |
                                                +----------------------+
                                                            |
                                                            v (data ring)
                                                    +-------+-----------+
                                                    |  @0->4098: in     |
                                                    |  ref[0]           |
                                                    |-------------------|
                                                    |  @4099->8196: out |
                                                    |  ref[1]           |
                                                    +-------------------+
 

#### Indexes Page Structure

    typedef uint32_t PVCALLS_RING_IDX;

    struct pvcalls_data_intf {
    	PVCALLS_RING_IDX in_cons, in_prod;
    	int32_t in_error;

    	uint8_t pad[52];

    	PVCALLS_RING_IDX out_cons, out_prod;
    	int32_t out_error;

    	uint8_t pad[52];

    	uint32_t ring_order;
    	grant_ref_t ref[];
    };

    /* not actually C compliant (ring_order changes from socket to socket) */
    struct pvcalls_data {
        char in[((1<<ring_order)<<PAGE_SHIFT)/2];
        char out[((1<<ring_order)<<PAGE_SHIFT)/2];
    };

- **ring_order**
  It represents the order of the data ring. The following list of grant
  references is of `(1 << ring_order)` elements. It cannot be greater than
  **max-page-order**, as specified by the backend on XenBus. It has to
  be one at minimum.
- **ref[]**
  The list of grant references which will contain the actual data. They are
  mapped contiguosly in virtual memory. The first half of the pages is the
  **in** array, the second half is the **out** array. The arrays must
  have a power of two size. Together, their size is `(1 << ring_order) *
  PAGE_SIZE`.
- **in** is an array used as circular buffer
  It contains data read from the socket. The producer is the backend, the
  consumer is the frontend.
- **out** is an array used as circular buffer
  It contains data to be written to the socket. The producer is the frontend,
  the consumer is the backend.
- **in_cons** and **in_prod**
  Consumer and producer indexes for data read from the socket. They keep track
  of how much data has already been consumed by the frontend from the **in**
  array. **in_prod** is increased by the backend, after writing data to **in**.
  **in_cons** is increased by the frontend, after reading data from **in**.
- **out_cons**, **out_prod**
  Consumer and producer indexes for the data to be written to the socket. They
  keep track of how much data has been written by the frontend to **out** and
  how much data has already been consumed by the backend. **out_prod** is
  increased by the frontend, after writing data to **out**. **out_cons** is
  increased by the backend, after reading data from **out**.
- **in_error** and **out_error** They signal errors when reading from the socket
  (**in_error**) or when writing to the socket (**out_error**). 0 means no
  errors. When an error occurs, no further reads or writes operations are
  performed on the socket. In the case of an orderly socket shutdown (i.e. read
  returns 0) **in_error** is set to ENOTCONN. **in_error** and **out_error**
  are never set to EAGAIN or EWOULDBLOCK (the data is written to the
  ring as soon as it is available).

The binary layout of `struct pvcalls_data_intf` follows:

    0         4         8         12           64        68        72        76 
    +---------+---------+---------+-----//-----+---------+---------+---------+
    | in_cons | in_prod |in_error |  padding   |out_cons |out_prod |out_error|
    +---------+---------+---------+-----//-----+---------+---------+---------+

    76        80        84        88      4092      4096
    +---------+---------+---------+----//---+---------+
    |ring_orde|  ref[0] |  ref[1] |         |  ref[N] |
    +---------+---------+---------+----//---+---------+

**N.B** For one page, N is maximum 991 ((4096-132)/4), but given that N needs
to be a power of two, actually max N is 512 (ring_order = 9).

#### Data Ring Structure

The binary layout of the data ring follow:

    0         ((1<<ring_order)<<PAGE_SHIFT)/2       ((1<<ring_order)<<PAGE_SHIFT)
    +------------//-------------+------------//-------------+
    |            in             |           out             |
    +------------//-------------+------------//-------------+

#### Why ring.h is not needed

Many Xen PV protocols use the macros provided by [ring.h] to manage
their shared ring for communication. PVCalls does not, because the [Data
Ring Structure] actually comes with two rings: the **in** ring and the
**out** ring. Each of them is mono-directional, and there is no static
request size: the producer writes opaque data to the ring. On the other
end, in [ring.h] they are combined, and the request size is static and
well-known. In PVCalls:

  in -> backend to frontend only
  out-> frontend to backend only

In the case of the **in** ring, the frontend is the consumer, and the
backend is the producer. Everything is the same but mirrored for the
**out** ring.

The producer, the backend in this case, never reads from the **in**
ring. In fact, the producer doesn't need any notifications unless the
ring is full. This version of the protocol doesn't take advantage of it,
leaving room for optimizations.

On the other end, the consumer always requires notifications, unless it
is already actively reading from the ring. The producer can figure it
out, without any additional fields in the protocol, by comparing the
indexes at the beginning and the end of the function. This is similar to
what [ring.h] does.

#### Workflow

The **in** and **out** arrays are used as circular buffers:
    
    0                               sizeof(array) == ((1<<ring_order)<<PAGE_SHIFT)/2
    +-----------------------------------+
    |to consume|    free    |to consume |
    +-----------------------------------+
               ^            ^
               prod         cons

    0                               sizeof(array)
    +-----------------------------------+
    |  free    | to consume |   free    |
    +-----------------------------------+
               ^            ^
               cons         prod

The following function is provided to calculate how many bytes are currently
left unconsumed in an array:

    #define _MASK_PVCALLS_IDX(idx, ring_size) ((idx) & (ring_size-1))

    static inline PVCALLS_RING_IDX pvcalls_ring_unconsumed(PVCALLS_RING_IDX prod,
    		PVCALLS_RING_IDX cons,
    		PVCALLS_RING_IDX ring_size)
    {
    	PVCALLS_RING_IDX size;
    
    	if (prod == cons)
    		return 0;
    
    	prod = _MASK_PVCALLS_IDX(prod, ring_size);
    	cons = _MASK_PVCALLS_IDX(cons, ring_size);
    
    	if (prod == cons)
    		return ring_size;
    
    	if (prod > cons)
    		size = prod - cons;
    	else {
    		size = ring_size - cons;
    		size += prod;
    	}
    	return size;
    }

The producer (the backend for **in**, the frontend for **out**) writes to the
array in the following way:

- read *[in|out]_cons*, *[in|out]_prod*, *[in|out]_error* from shared memory
- general memory barrier
- return on *[in|out]_error*
- write to array at position *[in|out]_prod* up to *[in|out]_cons*,
  wrapping around the circular buffer when necessary
- write memory barrier
- increase *[in|out]_prod*
- notify the other end via evtchn

The consumer (the backend for **out**, the frontend for **in**) reads from the
array in the following way:

- read *[in|out]_prod*, *[in|out]_cons*, *[in|out]_error* from shared memory
- read memory barrier
- return on *[in|out]_error*
- read from array at position *[in|out]_cons* up to *[in|out]_prod*,
  wrapping around the circular buffer when necessary
- general memory barrier
- increase *[in|out]_cons*
- notify the other end via evtchn

The producer takes care of writing only as many bytes as available in
the buffer up to *[in|out]_cons*. The consumer takes care of reading
only as many bytes as available in the buffer up to *[in|out]_prod*.
*[in|out]_error* is set by the backend when an error occurs writing or
reading from the socket.


[xenstore]: http://xenbits.xen.org/docs/unstable/misc/xenstore.txt
[XenbusStateInitialising]: http://xenbits.xen.org/docs/unstable/hypercall/x86_64/include,public,io,xenbus.h.html
[address]: http://pubs.opengroup.org/onlinepubs/7908799/xns/syssocket.h.html
[in]: http://pubs.opengroup.org/onlinepubs/000095399/basedefs/netinet/in.h.html
[socket]: http://pubs.opengroup.org/onlinepubs/009695399/functions/socket.html
[connect]: http://pubs.opengroup.org/onlinepubs/7908799/xns/connect.html
[shutdown]: http://pubs.opengroup.org/onlinepubs/7908799/xns/shutdown.html
[bind]: http://pubs.opengroup.org/onlinepubs/7908799/xns/bind.html
[listen]: http://pubs.opengroup.org/onlinepubs/7908799/xns/listen.html
[accept]: http://pubs.opengroup.org/onlinepubs/7908799/xns/accept.html
[poll]: http://pubs.opengroup.org/onlinepubs/7908799/xsh/poll.html
[ring.h]: http://xenbits.xen.org/gitweb/?p=xen.git;a=blob;f=xen/include/public/io/ring.h;hb=HEAD
