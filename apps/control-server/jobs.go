package main

import (
	"context"
	"errors"
	"sync"
	"time"
)

type JobStatus string

const (
	JobQueued    JobStatus = "queued"
	JobRunning   JobStatus = "running"
	JobSucceeded JobStatus = "succeeded"
	JobFailed    JobStatus = "failed"
	JobCancelled JobStatus = "cancelled"
)

type ImportJob struct {
	ID        string     `json:"id"`
	Status    JobStatus  `json:"status"`
	Phase     string     `json:"phase"`
	Progress  int        `json:"progress"`
	Message   string     `json:"message"`
	CreatedAt time.Time  `json:"createdAt"`
	UpdatedAt time.Time  `json:"updatedAt"`
	Map       *MapRecord `json:"map,omitempty"`
	Error     string     `json:"error,omitempty"`
}

type JobProgress struct {
	Phase    string
	Progress int
	Message  string
}

type importWork func(context.Context, func(JobProgress)) (MapRecord, error)

type jobEntry struct {
	job         ImportJob
	cancel      context.CancelFunc
	subscribers map[chan ImportJob]struct{}
}

type JobManager struct {
	mu      sync.RWMutex
	entries map[string]*jobEntry
	workers chan struct{}
}

func NewJobManager(maxWorkers int) *JobManager {
	if maxWorkers <= 0 {
		maxWorkers = 1
	}
	return &JobManager{
		entries: make(map[string]*jobEntry),
		workers: make(chan struct{}, maxWorkers),
	}
}

func (m *JobManager) Submit(work importWork) ImportJob {
	now := time.Now().UTC()
	ctx, cancel := context.WithCancel(context.Background())
	entry := &jobEntry{
		job: ImportJob{
			ID: newID("job"), Status: JobQueued, Phase: "queued", Progress: 0,
			Message: "等待地图编译工作线程", CreatedAt: now, UpdatedAt: now,
		},
		cancel:      cancel,
		subscribers: make(map[chan ImportJob]struct{}),
	}
	m.mu.Lock()
	m.entries[entry.job.ID] = entry
	initial := cloneJob(entry.job)
	m.mu.Unlock()

	go m.run(ctx, initial.ID, work)
	return initial
}

func (m *JobManager) run(ctx context.Context, id string, work importWork) {
	select {
	case m.workers <- struct{}{}:
		defer func() { <-m.workers }()
	case <-ctx.Done():
		m.finish(id, JobCancelled, nil, ctx.Err())
		return
	}

	m.update(id, func(job *ImportJob) {
		job.Status = JobRunning
		job.Phase = "preparing"
		job.Progress = 5
		job.Message = "准备地图编译目录"
	})
	record, err := work(ctx, func(progress JobProgress) {
		m.update(id, func(job *ImportJob) {
			job.Phase = progress.Phase
			job.Progress = min(max(progress.Progress, job.Progress), 99)
			job.Message = progress.Message
		})
	})
	if err != nil {
		status := JobFailed
		if errors.Is(ctx.Err(), context.Canceled) {
			status = JobCancelled
		}
		m.finish(id, status, nil, err)
		return
	}
	m.finish(id, JobSucceeded, &record, nil)
}

func (m *JobManager) finish(id string, status JobStatus, record *MapRecord, err error) {
	m.update(id, func(job *ImportJob) {
		job.Status = status
		job.Phase = string(status)
		if status == JobSucceeded {
			job.Progress = 100
			job.Message = "运行时地图已发布"
			job.Map = record
		} else if status == JobCancelled {
			job.Message = "地图编译已取消"
		} else {
			job.Message = "地图编译失败"
		}
		if err != nil {
			job.Error = err.Error()
		}
	})
}

func (m *JobManager) Get(id string) (ImportJob, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	entry, ok := m.entries[id]
	if !ok {
		return ImportJob{}, false
	}
	return cloneJob(entry.job), true
}

func (m *JobManager) Cancel(id string) (ImportJob, bool) {
	m.mu.RLock()
	entry, ok := m.entries[id]
	if !ok {
		m.mu.RUnlock()
		return ImportJob{}, false
	}
	entry.cancel()
	job := cloneJob(entry.job)
	m.mu.RUnlock()
	return job, true
}

func (m *JobManager) Subscribe(id string) (ImportJob, <-chan ImportJob, func(), bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	entry, ok := m.entries[id]
	if !ok {
		return ImportJob{}, nil, nil, false
	}
	updates := make(chan ImportJob, 1)
	entry.subscribers[updates] = struct{}{}
	unsubscribe := func() {
		m.mu.Lock()
		defer m.mu.Unlock()
		if current, exists := m.entries[id]; exists {
			delete(current.subscribers, updates)
		}
	}
	return cloneJob(entry.job), updates, unsubscribe, true
}

func (m *JobManager) update(id string, mutate func(*ImportJob)) {
	m.mu.Lock()
	defer m.mu.Unlock()
	entry, ok := m.entries[id]
	if !ok {
		return
	}
	mutate(&entry.job)
	entry.job.UpdatedAt = time.Now().UTC()
	snapshot := cloneJob(entry.job)
	for subscriber := range entry.subscribers {
		select {
		case subscriber <- snapshot:
		default:
			select {
			case <-subscriber:
			default:
			}
			select {
			case subscriber <- snapshot:
			default:
			}
		}
	}
}

func cloneJob(job ImportJob) ImportJob {
	clone := job
	if job.Map != nil {
		record := *job.Map
		record.Issues = append([]ValidationIssue(nil), job.Map.Issues...)
		clone.Map = &record
	}
	return clone
}

func terminalJob(status JobStatus) bool {
	return status == JobSucceeded || status == JobFailed || status == JobCancelled
}
