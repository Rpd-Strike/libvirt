=======================
Firecracker driver
=======================

.. contents::

`Firecracker <https://github.com/firecracker-microvm/firecracker>`__ is an open source Virtual Machine Monitor (VMM) that
runs on top of KVM. The focus of the project is on running VM's in a
light-weight, secure way providing multi-tenant support. The features
supported are minimalist to ensure the performance and security standards.

The libvirt Firecrakcer driver is intended to be run as a system
driver. The firecracker binary itself should be
available in the path.

Expected connection URI would be:

``fc:///system``

The driver is supported on all versions above and including ``v0.25`` but strictly below ``v1.0``.

Example guest domain XML configurations
=======================================

The Firecracker driver in libvirt is in its early stage of development
supporting a subset of the features provided by the Firecracker VMM, for now being
more a proof of concept.

Firecracker binaries can be found on the
`github repository <https://github.com/firecracker-microvm/firecracker/releases>`__.

Example of XML configuration:

::

  <domain type='firecracker'>
    <name>demo_domain</name>
    <memory unit='GiB'>1</memory>
    <vcpu>4</vcpu>
    <os>
      <type arch='x86_64' machine='pc'>hvm</type>
      <kernel>/home/user/kernels/hello-vmlinux.bin</kernel>
      <cmdline>panic=1</cmdline>
      <root>vda</root>
    </os>
    <devices>
      <disk type='file' device='disk'>
        <source file='/home/ec2-user/LV/myownfiles/isos/hello-rootfs.ext4'/>
        <target dev='vda' bus='virtio' />
      </disk>

      <serial type='pty'>
        <target port='0'/>
      </serial>

      <interface type='ethernet'>
        <target dev='tap0'/>
        <guest dev='eth0'/>
      </interface>

      <interface type='ethernet'>
        <target dev='tap2'/>
        <guest dev='eth1'/>>
      </interface>
    </devices>
  </domain>

In the above example we specified the domain type to be `firecracker`,
we specified the name of the domain, together with the memory and vcpus allocated.
In the `<os>` tag we specified the source file for the kernel binary, the kernel command line.
The only architecture supported for now is `x86_64` with the `hvm` type.
The `<root>` tag specifies which of the following disk devices is used as the rootfs by matching the
dev mount point attribute in the `<target>` tag. One way to build a kernel and a rootfs working can be found `here <https://github.com/firecracker-microvm/firecracker/blob/main/docs/rootfs-and-kernel-setup.md>`__.

For the devices section:
The only disk device is actually the rootfs, as it corresponds to the <root>
tag from the os definition.
There is one serial device of type `pty` with target port `0`. However, depending on the version of firecracker the target port is
not relevant.
Below there are 2 net interfaces defined of type ethernet, each connected
to already existing tap devices.

XML structure:
===============

``<domain>``
   The domain type should be ``firecracker``.

``<name>``
   The name tag specifies the name of the vm.

``<memory>``
   The memory tag specifies the amount of RAM given to the VM.

``<vcpu>``
   The number of vcpu threads allocated for the vm.

``<os>``
   Includes information about os configuration
   Currently only ``x86_64`` architecture is supported, having ``hvm`` type.

   ``<kernel>``
      Path to the kernel binary the vm will run on.

   ``<cmdline>``
      The command line arguments the vm will boot with.

   ``<root>``
      The value of this tag tells the driver which one of the disk devices.
      is the rootfs one. It matches this value with the attribute `dev` on the ``<target>`` tag of
      the device.

``<devices>``
   Devices supported include disk devices, maximum one serial device and ethernet interfaces based on tap interfaces.

   **Disk devices:**

   Currently for disk devices, only one disk device is supported, the rootfs, one example below:

   ::

      <disk type='file' device='disk'>
         <source file='/home/ec2-user/LV/myownfiles/isos/hello-rootfs.ext4'/>
         <target dev='vda' bus='virtio' />
         <readonly/>
      <disk>


   ``<source>``
      Mandatory tag that specifies the path to the backing file for the disk device.

   ``<target>``
      Mandatory tag that specifies the drive id under the ``dev`` attribute.
      Currently only virtio over mmio devices are supported, so this tag should be present: ``bus='virtio'``.

   ``<readonly/>``
      Optional tag that specifies if the disk device should be mounted in read-only mode.

   **Serial devices:**

   Firecracker supports only one serial console device. If you wish to use make the serial device available
   you need to specify it using a ``<serial>`` device. An example below:

   ::

      <serial type='pty'>
         <target port='0'/>
      </serial>


   ``<target>``
      Optional tag that specifies the port to be used for the serial. However this property is currently ignored in firecracker `v0.*`.

   **Ethernet devices:**

   Firecracker supports simulating ethernet interfaces based on tap devices on the host. One example below:

   ::

      <interface type='ethernet'>
         <target dev='tap0'/>
         <guest dev='eth0'/>
      </interface>

   The only supported interface is ``ethernet``.

   ``<target>``
      Tag that specifies through the ``dev`` attribute what tap device on the host to use.

   ``<guest>``
      Tag that specifies through the ``dev`` attribute what interface name the ethernet device should have on the guest.
