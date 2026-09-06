#include "stdafx.h"

#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

#define _ThrowLastError throw ::GetLastError()
#define _ThrowCode(__c) throw __c
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd), buff_size(bs), sa(s) {};

PipeChannelBase::~PipeChannelBase() {
  // Thread-specific pointers are cleaned up automatically
}

bool PipeChannelBase::QueryServerIdentity(DWORD& processId,
                                          DWORD& sessionId,
                                          DWORD& stage,
                                          DWORD& error) const {
  // Preserve the caller's last-error state; diagnostics are explicit outputs.
  const DWORD savedError = ::GetLastError();
  struct LastErrorScope {
    DWORD value;
    ~LastErrorScope() { ::SetLastError(value); }
  } restore{savedError};
  processId = 0;
  sessionId = 0;
  stage = 1;
  error = ERROR_PIPE_NOT_CONNECTED;
  const HANDLE* pipe = hpipe_ptr.get();
  if (!pipe || !*pipe || _Invalid(*pipe))
    return false;

  // This pipe is thread-local and is not being read by a second thread.
  // Peek consumes no response bytes and sends no request to the server.
  stage = 2;
  if (!::PeekNamedPipe(*pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
    error = ::GetLastError();
    return false;
  }
  ULONG peerPid = 0;
  ULONG peerSession = 0;
  stage = 3;
  if (!::GetNamedPipeServerProcessId(*pipe, &peerPid)) {
    error = ::GetLastError();
    return false;
  }
  stage = 4;
  if (!::GetNamedPipeServerSessionId(*pipe, &peerSession)) {
    error = ::GetLastError();
    return false;
  }
  stage = 5;
  if (!peerPid || peerPid == ::GetCurrentProcessId()) {
    error = ERROR_INVALID_DATA;
    return false;
  }
  DWORD ourSession = 0;
  stage = 6;
  if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &ourSession)) {
    error = ::GetLastError();
    return false;
  }
  stage = 7;
  if (peerSession != ourSession) {
    error = ERROR_ACCESS_DENIED;
    return false;
  }
  processId = peerPid;
  sessionId = peerSession;
  stage = 100;
  error = ERROR_SUCCESS;
  return true;
}

bool PipeChannelBase::_Ensure() {
  try {
    HANDLE* phandle = _GetPipeHandle();
    if (_Invalid(*phandle)) {
      *phandle = _Connect(pname.c_str());
      return !_Invalid(*phandle);
    }
  } catch (...) {
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  while (_Invalid(pipe = _TryConnect()))
    ::WaitNamedPipe(name, 500);
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  HANDLE* phandle = _GetPipeHandle();
  _FinalizePipe(*phandle);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b) {
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  ::FlushFileBuffers(pipe);
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  DWORD lread;
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  if (!success) {
    _ThrowIfNot(ERROR_MORE_DATA);

    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  _GetContext()->has_body = false;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe =
      CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                      PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (pipe == INVALID_HANDLE_VALUE || !::ConnectNamedPipe(pipe, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}
