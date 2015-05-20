// clang++ main.cpp -o main && ./main

#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <unistd.h>
#include <iostream>

#define CHECK_MACH_ERROR(err, msg)                                      \
    if (err != KERN_SUCCESS) {                                          \
        mach_error (msg, err);                                          \
        return -1;                                                      \
    }                                                                   \


static task_t       child_task = MACH_PORT_NULL;


static bool
CreateThePort(mach_vm_address_t& child_address)
{
  mach_vm_address_t address;
  mach_port_t port;
  size_t size = 8000;

  kern_return_t kr = mach_vm_allocate(mach_task_self(), &address, round_page(size), VM_FLAGS_ANYWHERE);
  if (kr != KERN_SUCCESS) {
    printf("Failed to allocate mach_vm_allocate shared memory (%zu bytes). %s (%x)", size, mach_error_string(kr), kr);
    return false;
  }



  memory_object_size_t memoryObjectSize = round_page(size);

  kr = mach_make_memory_entry_64(mach_task_self(), &memoryObjectSize, address, VM_PROT_DEFAULT, &port, MACH_PORT_NULL);
  if (kr != KERN_SUCCESS) {
    printf("Failed to make memory entry (%zu bytes). %s (%x)\n", size, mach_error_string(kr), kr);
    return false;
  }

  vm_prot_t vmProtection = VM_PROT_READ | VM_PROT_WRITE;

  kr = mach_vm_map(child_task, &child_address, round_page(size), 0, VM_FLAGS_ANYWHERE,
                  port, 0, false, vmProtection, vmProtection, VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) {
    printf("Failed to mach_vm_map (%zu bytes). %s (%x)\n", size, mach_error_string(kr), kr);
    return false;
  }

  int* buf = reinterpret_cast<int*>(static_cast<uintptr_t>(address));

  buf[0] = 42;

  printf("address: %p\n", reinterpret_cast<int*>(static_cast<uintptr_t>(address)));
  printf("child_address: %p\n", reinterpret_cast<int*>(static_cast<uintptr_t>(child_address)));

  printf("create port %x\n", port);
  return true;
}

static int
setup_recv_port (mach_port_t *recv_port)
{
    kern_return_t       err;
    mach_port_t         port = MACH_PORT_NULL;
    err = mach_port_allocate (mach_task_self (),
                              MACH_PORT_RIGHT_RECEIVE, &port);
    CHECK_MACH_ERROR (err, "mach_port_allocate failed:");

    err = mach_port_insert_right (mach_task_self (),
                                  port,
                                  port,
                                  MACH_MSG_TYPE_MAKE_SEND);
    CHECK_MACH_ERROR (err, "mach_port_insert_right failed:");

    *recv_port = port;
    return 0;
}

static int
send_port (mach_port_t remote_port, mach_port_t port)
{
    kern_return_t       err;

    struct {
        mach_msg_header_t          header;
        mach_msg_body_t            body;
        mach_msg_port_descriptor_t task_port;
    } msg;

    msg.header.msgh_remote_port = remote_port;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_bits = MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND, 0) |
        MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof msg;

    msg.body.msgh_descriptor_count = 1;
    msg.task_port.name = port;
    msg.task_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.task_port.type = MACH_MSG_PORT_DESCRIPTOR;

    err = mach_msg_send (&msg.header);
    CHECK_MACH_ERROR (err, "mach_msg_send failed:");

    return 0;
}

static int
recv_port (mach_port_t recv_port, mach_port_t *port)
{
    kern_return_t       err;
    struct {
        mach_msg_header_t          header;
        mach_msg_body_t            body;
        mach_msg_port_descriptor_t task_port;
        mach_msg_trailer_t         trailer;
    } msg;

    err = mach_msg (&msg.header, MACH_RCV_MSG,
                    0, sizeof msg, recv_port,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    CHECK_MACH_ERROR (err, "mach_msg failed:");

    *port = msg.task_port.name;
    return 0;
}

static void
RunChildProcess(int aSourceFromParent)
{
  std::cout << "Running the child process" << std::endl;
  mach_vm_address_t child_address;
  size_t numRead = read(aSourceFromParent, &child_address, sizeof(child_address));
  if (numRead > 0) {
    std::cout << "Read from parent: " << reinterpret_cast<int*>(static_cast<uintptr_t>(child_address)) << std::endl;

  } else {
    std::cout << "Nothing to read" << std::endl;
  }

}

static void
RunParentProcess(pid_t aChildProcessPID, int aSinkToChild)
{
  std::cout << "Creating a port in the parent process" << std::endl;
  mach_vm_address_t child_address;
  if (!CreateThePort(child_address)) {
    std::cout << "Failed!" << std::endl;
  } else {
    std::cout << "Succeeded! with address " << reinterpret_cast<int*>(static_cast<uintptr_t>(child_address)) << " and content " << *reinterpret_cast<int*>(static_cast<uintptr_t>(child_address)) << std::endl;
  }
  write(aSinkToChild, &child_address, sizeof(child_address));
}

pid_t
sampling_fork ()
{
    kern_return_t       err;
    mach_port_t         parent_recv_port = MACH_PORT_NULL;
    mach_port_t         child_recv_port = MACH_PORT_NULL;

    if (setup_recv_port (&parent_recv_port) != 0)
        return -1;
    err = task_set_bootstrap_port (mach_task_self (), parent_recv_port);
    CHECK_MACH_ERROR (err, "task_set_bootstrap_port failed:");

    pid_t               pid;
    switch (pid = fork ()) {
    case -1:
        err = mach_port_deallocate (mach_task_self(), parent_recv_port);
        CHECK_MACH_ERROR (err, "mach_port_deallocate failed:");
        return pid;
    case 0: /* child */
        err = task_get_bootstrap_port (mach_task_self (), &parent_recv_port);
        CHECK_MACH_ERROR (err, "task_get_bootstrap_port failed:");
        if (setup_recv_port (&child_recv_port) != 0)
            return -1;
        if (send_port (parent_recv_port, mach_task_self ()) != 0)
            return -1;
        if (send_port (parent_recv_port, child_recv_port) != 0)
            return -1;
        if (recv_port (child_recv_port, &bootstrap_port) != 0)
            return -1;
        err = task_set_bootstrap_port (mach_task_self (), bootstrap_port);
        CHECK_MACH_ERROR (err, "task_set_bootstrap_port failed:");
        break;
    default: /* parent */
        err = task_set_bootstrap_port (mach_task_self (), bootstrap_port);
        CHECK_MACH_ERROR (err, "task_set_bootstrap_port failed:");
        if (recv_port (parent_recv_port, &child_task) != 0)
            return -1;
        if (recv_port (parent_recv_port, &child_recv_port) != 0)
            return -1;
        if (send_port (child_recv_port, bootstrap_port) != 0)
            return -1;
        err = mach_port_deallocate (mach_task_self(), parent_recv_port);
        CHECK_MACH_ERROR (err, "mach_port_deallocate failed:");
        break;
    }

    return pid;
}

int main()
{
  int channel[2];
  pipe(channel);
  pid_t subprocessId = sampling_fork();
  if (subprocessId == 0) {
    RunChildProcess(channel[0]);
  } else {
    RunParentProcess(subprocessId, channel[1]);
  }
  return 0;
}