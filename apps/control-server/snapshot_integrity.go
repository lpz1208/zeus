package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// A contract version, not a claim that arbitrary future engine binaries replay
// identically. Bump when replay semantics change incompatibly.
const agentReplayContract = "zeus-session-replay-v3"
const agentSnapshotMaxBytes = 64 << 20

func snapshotDigest(artifact agentSnapshotArtifact) (string, error) {
	artifact.Checksum = ""
	data, err := json.Marshal(artifact)
	if err != nil {
		return "", err
	}
	if len(data) > agentSnapshotMaxBytes {
		return "", errors.New("agent snapshot exceeds 64 MiB")
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), nil
}

func validSHA256(value string) bool {
	bytes, err := hex.DecodeString(value)
	return err == nil && len(bytes) == sha256.Size && value == hex.EncodeToString(bytes)
}

func sealAgentSnapshot(artifact *agentSnapshotArtifact, revision string) error {
	if !validSHA256(revision) {
		return errors.New("session has no verified map revision")
	}
	artifact.FormatVersion = agentSnapshotFormatVersion
	artifact.MapRevision = revision
	artifact.ReplayContract = agentReplayContract
	digest, err := snapshotDigest(*artifact)
	if err != nil {
		return err
	}
	artifact.Checksum = digest
	// Persistence uses indented JSON; enforce the reader's limit on that exact
	// representation, including the checksum, rather than just compact bytes.
	data, err := json.MarshalIndent(artifact, "", "  ")
	if err != nil {
		return err
	}
	if len(data) > agentSnapshotMaxBytes {
		return errors.New("agent snapshot exceeds 64 MiB")
	}
	return nil
}

func verifyAgentSnapshot(artifact agentSnapshotArtifact) error {
	switch artifact.FormatVersion {
	case 1, 2:
		if artifact.Checksum != "" || artifact.MapRevision != "" || artifact.ReplayContract != "" {
			return errors.New("legacy snapshot contains inconsistent integrity metadata")
		}
		return nil
	case agentSnapshotFormatVersion:
		if !validSHA256(artifact.MapRevision) || !validSHA256(artifact.Checksum) {
			return errors.New("snapshot integrity metadata is missing or invalid")
		}
		digest, err := snapshotDigest(artifact)
		if err != nil {
			return err
		}
		if digest != artifact.Checksum {
			return errors.New("snapshot checksum mismatch; restore refused")
		}
		if artifact.ReplayContract != agentReplayContract {
			return errors.New("snapshot replay contract is unsupported")
		}
		return nil
	default:
		return errors.New("unsupported agent snapshot format")
	}
}

// Pin exactly the bytes hashed into an immutable, content-addressed path. The
// resident worker is keyed by this path, so replacing map.zmap cannot reuse a
// worker that loaded a different version. Copies are retained across restarts.
func (s *Server) pinAgentRuntime(ctx context.Context, record *MapRecord, expected string) error {
	source, err := os.Open(record.Runtime)
	if err != nil {
		return fmt.Errorf("open session map: %w", err)
	}
	defer source.Close()
	before, err := source.Stat()
	if err != nil {
		return err
	}
	if !before.Mode().IsRegular() {
		return errors.New("session map is not a regular file")
	}
	dir := filepath.Join(s.mapsDir(), record.ID, "runtime-revisions")
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	copy, err := os.CreateTemp(dir, ".runtime-*")
	if err != nil {
		return err
	}
	defer os.Remove(copy.Name())
	defer copy.Close()
	digest := sha256.New()
	buffer := make([]byte, 256<<10)
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		count, readErr := source.Read(buffer)
		if count > 0 {
			if _, err := copy.Write(buffer[:count]); err != nil {
				return err
			}
			_, _ = digest.Write(buffer[:count])
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return readErr
		}
	}
	after, err := source.Stat()
	if err != nil {
		return err
	}
	if before.Size() != after.Size() || !before.ModTime().Equal(after.ModTime()) {
		return errors.New("map changed while preparing session; retry")
	}
	revision := hex.EncodeToString(digest.Sum(nil))
	if expected != "" && expected != revision {
		return errors.New("snapshot map revision differs from current map; restore refused")
	}
	if err := copy.Sync(); err != nil {
		return err
	}
	if err := copy.Chmod(0400); err != nil {
		return err
	}
	if err := copy.Close(); err != nil {
		return err
	}
	path := filepath.Join(dir, revision+".zmap")
	if err := os.Rename(copy.Name(), path); err != nil {
		return err
	}
	record.Runtime, record.RuntimeRevision = path, revision
	return nil
}
