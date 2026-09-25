"""Host escape attempts, resource accounting and routing language behavior."""
import importlib.util
from pathlib import Path
import pytest
import io
import json

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('zeus_routing_language', ROOT / 'apps/control-server/algorithm_runner.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

@pytest.fixture
def graph():
    return {'adjacency': {'-1': [[0,0,2,20],[1,1,8,80]], '0': [[1,1,3,30]], '1': [[-2,2,1,10]]},
            'nodes':[1,2,3], 'estimates':[4,1,0]}

def test_template_finds_cheapest_path_and_records_search(graph):
    result=runner.execute(graph,(ROOT/'apps/control-server/algorithm_template.py').read_text())
    assert result['ok']
    assert result['states']==[-1,0,1,-2]
    assert result['searchTrace']['steps'][1]['g']==5

@pytest.mark.parametrize('body',[
    'import os', 'return open("/etc/passwd")', 'return eval("1")',
    'return ctx.graph', 'return ctx.__class__', 'return getattr(ctx, "graph")',
    'return __import__("os")', 'return ctx.method("graph")',
    'return [x for x in range(10)]', 'return (lambda: 1)()',
    'return "x" * 100000000', 'return 2 ** 999999',
])
def test_language_rejects_host_access_and_unbounded_primitives(graph,body):
    result=runner.execute(graph,'def route(ctx):\n    '+body)
    assert not result['ok']
    assert result['line']>=1

def test_mutating_neighbors_cannot_modify_snapshot(graph):
    code='''def route(ctx):
    a = ctx.neighbors(ctx.start())
    a[0]["cost"] = -100
    b = ctx.neighbors(ctx.start())
    ctx.log(b[0]["cost"])
    return None
'''
    result=runner.execute(graph,code)
    assert result['ok'] and result['logs']==['2']
    assert graph['adjacency']['-1'][0][2]==2


def test_observation_is_a_deep_copy_and_keep_is_explicit(graph):
    graph['observation'] = {'mode': 'vehicle', 'stateVersion': 12, 'vehicle': {'routeInvalidated': False, 'remainingEdgeIds': [0, 1]}}
    result = runner.execute(graph, '''def route(ctx):
    observation = ctx.observation()
    observation["vehicle"]["remainingEdgeIds"][0] = 99
    ctx.log(ctx.observation()["vehicle"]["remainingEdgeIds"][0])
    ctx.log(ctx.observation()["stateVersion"])
    return ctx.keep()
''')
    assert result['ok'] and result['keepCurrentRoute'] and result['states'] is None
    assert result['logs'] == ['0', '12']
    assert graph['observation']['vehicle']['remainingEdgeIds'] == [0, 1]


def test_static_observation_and_vehicle_only_keep(graph):
    result = runner.execute(graph, 'def route(ctx):\n    ctx.log(ctx.observation()["mode"])\n    return None')
    assert result['ok'] and result['logs'] == ['static'] and not result['keepCurrentRoute']
    result = runner.execute(graph, 'def route(ctx):\n    return ctx.keep()')
    assert not result['ok'] and '仅用于车辆模式' in result['error']


def test_repeated_observation_copy_consumes_budget(graph):
    graph['observation'] = {'mode': 'vehicle', 'edges': list(range(2000))}
    result = runner.execute(graph, 'def route(ctx):\n    return ctx.observation()', steps=1000)
    assert not result['ok'] and '步数' in result['error']

@pytest.mark.parametrize('body', [
    'while True:\n        pass',
    'return route(ctx)',
    'items = []\n    for i in range(200000):\n        items.append(i)',
])
def test_infinite_or_expensive_program_is_bounded(graph,body):
    result=runner.execute(graph,'def route(ctx):\n    '+body,steps=1000)
    assert not result['ok']
    assert result['steps']<=1010


def test_logs_and_traces_are_capped_without_affecting_search(graph):
    code='''def route(ctx):
    for i in range(6000):
        ctx.record(0, i, i)
        ctx.log(i)
    return ctx.route([-1, 0, 1, -2])
'''
    result=runner.execute(graph,code)
    assert result['ok'] and len(result['logs'])==100
    assert len(result['searchTrace']['steps'])==5000
    assert result['searchTrace']['sampled']
    assert result['searchTrace']['stepCount']==6000


def test_stable_queue_and_helper_function(graph):
    code='''def identity(x):
    return x

def route(ctx):
    q = ctx.queue()
    q.push(1, 10)
    q.push(0, 10)
    state, priority = q.pop()
    ctx.log(identity(state))
    ctx.log(ctx.estimate(0))
    return None
'''
    result=runner.execute(graph,code)
    assert result['ok'] and result['logs']==['1','4']


def test_helper_cannot_break_callers_loop(graph):
    code='''def stop():
    break

def route(ctx):
    while True:
        stop()
    return None
'''
    result=runner.execute(graph,code)
    assert not result['ok']
    assert '当前函数' in result['error']


@pytest.mark.parametrize('source', ['def route(ctx):\n    import os', 'def route(ctx):\n    return open("x")', 'def route():\n    return None', 'def route(ctx)\n    return None'])
def test_static_checks_without_executing(source):
    result = runner.check_source(source)
    assert not result['ok'] and result['line'] >= 1


def test_static_check_does_not_run_infinite_loop():
    assert runner.check_source('def route(ctx):\n    while True:\n        pass')['ok']


def debug_run(graph, source, commands):
    output = io.StringIO()
    debugger = runner.Debugger(io.StringIO(''.join(json.dumps(command) + '\n' for command in commands)), output)
    result = runner.execute(graph, source, debugger=debugger)
    return result, [json.loads(line) for line in output.getvalue().splitlines()]


def test_debug_step_keeps_variables_and_continue_hits_breakpoint(graph):
    result, events = debug_run(graph, 'def route(ctx):\n    n = 1\n    n = n + 2\n    return None', [
        {'action': 'step'}, {'action': 'continue', 'breakpoints': [4]}, {'action': 'continue'},
    ])
    assert result['ok']
    assert [event['line'] for event in events] == [2, 3, 4]
    assert events[1]['variables']['n'] == '1'
    assert events[2]['variables']['n'] == '3'
    assert events[0]['variables']['ctx'] == '<只读路网 API>'


def test_loop_condition_breakpoint_hits_every_iteration(graph):
    result, events = debug_run(graph, 'def route(ctx):\n    n = 0\n    while n < 2:\n        n += 1\n    return None',
                               [{'action': 'continue', 'breakpoints': [3]}] * 3 + [{'action': 'continue'}])
    assert result['ok']
    assert [event['line'] for event in events] == [2, 3, 3, 3]
    assert [event['variables']['n'] for event in events[1:]] == ['0', '1', '2']


def test_debug_step_enters_helper_and_returns_to_caller(graph):
    source = 'def helper(x):\n    return x + 1\n\ndef route(ctx):\n    n = helper(1)\n    return None'
    result, events = debug_run(graph, source, [{'action': 'step'}, {'action': 'step'}, {'action': 'continue'}])
    assert result['ok']
    assert [event['line'] for event in events] == [5, 2, 6]
    assert events[1]['frames'] == ['route', 'helper'] and events[1]['variables']['x'] == '1'
    assert events[2]['variables']['n'] == '2'


def test_debug_pause_does_not_consume_execution_time(graph, monkeypatch):
    clock = [0.0]
    monkeypatch.setattr(runner.time, 'monotonic', lambda: clock[0])
    class PausingInput:
        def readline(self, limit):
            clock[0] += 20
            return '{"action":"step"}\n'
    debugger = runner.Debugger(PausingInput(), io.StringIO())
    result = runner.execute(graph, 'def route(ctx):\n    n = 1\n    return None', debugger=debugger)
    assert result['ok'] and result['computeMs'] == 0
    assert clock[0] == 40


def test_debug_container_previews_are_bounded_and_handle_cycles():
    items = []
    items.append(items)
    assert len(runner.debug_value(items)) < 400
    assert len(runner.debug_value(list(range(200000)))) < 400
    assert '…' in runner.debug_value(list(range(20)))
