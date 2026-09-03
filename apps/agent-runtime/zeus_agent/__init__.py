"""Zeus A2 single navigation agent runtime.

HTTP-first transport against the Go control plane's agent session API; the
EnvironmentClient Protocol in zeus_agent.client is the seam a future gRPC
transport slots into. Wire shapes mirror tools/zeus-map/session_worker.cc.
"""

__all__ = ["client", "policy", "model", "graph", "runner", "gym"]
