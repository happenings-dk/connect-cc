# Local overlay recipe

Do not use this template directory directly as an overlay. Run
`../prepare.cmake` to copy the runtime into a fresh overlay directory. Including
the snapshot in the port makes vcpkg's source hash cover implementation changes.
The port builds a static runtime; install the Go generator separately.
