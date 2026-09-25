"""Zeus routing language v1: interpret a small Python AST, never exec/eval it.

Only scalar/container values, user functions and explicitly dispatched host
methods exist in the language. No Python object attributes are exposed.
"""
import ast
import heapq
import json
import math
import resource
import sys
import time

MAX_CONTAINER = 200000
MAX_ALLOCATIONS = 2000000


class LabError(Exception):
    pass


class Returned(Exception):
    def __init__(self, value):
        self.value = value


class BreakLoop(Exception):
    pass


class ContinueLoop(Exception):
    pass


class Route:
    def __init__(self, states):
        self.states = states


class KeepRoute:
    pass


class Queue:
    def __init__(self):
        self.items = []
        self.order = 0


def debug_value(value, depth=0):
    """Bounded previews: never walk arbitrary attributes or expand cycles."""
    if type(value) is str:
        return repr(value[:120]) + ('…' if len(value) > 120 else '')
    if value is None or type(value) in (int, float, bool):
        return str(value)
    if isinstance(value, Queue):
        return 'queue(size=' + str(len(value.items)) + ')'
    if isinstance(value, Route):
        return 'route(states=' + str(len(value.states)) + ')'
    if isinstance(value, range):
        return 'range(size=' + str(len(value)) + ')'
    if isinstance(value, (list, tuple, dict)):
        if depth >= 2:
            return type(value).__name__ + '(size=' + str(len(value)) + ')'
        parts = []
        for index, item in enumerate(value):
            if index == 6:
                parts.append('…'); break
            text = debug_value(item, depth + 1)
            if isinstance(value, dict):
                text += ': ' + debug_value(value[item], depth + 1)
            parts.append(text)
        opening, closing = ('{', '}') if isinstance(value, dict) else ('[', ']') if isinstance(value, list) else ('(', ')')
        return (opening + ', '.join(parts) + closing)[:400]
    return '<只读路网 API>'


class Debugger:
    def __init__(self, input_stream, output_stream):
        self.input = input_stream
        self.output = output_stream
        self.mode = 'step'
        self.breakpoints = set()
        self.sequence = 0

    def before(self, node, scope, interpreter):
        if self.mode != 'step' and node.lineno not in self.breakpoints:
            return
        self.sequence += 1
        if self.sequence > 1000:
            raise LabError('调试暂停次数超过 1000，请减少断点或改用运行验证')
        paused_at = time.monotonic()
        variables = {}
        for index, (name, value) in enumerate(scope.items()):
            if index == 24: break
            variables[name[:64]] = debug_value(value)
        event = dict(kind='paused', line=node.lineno, sequence=self.sequence,
                     variables=variables, frames=[name[:64] for name in interpreter.stack],
                     steps=interpreter.steps, expandedNodes=interpreter.expansions,
                     logs=interpreter.logs[-10:])
        self.output.write(json.dumps(event, allow_nan=False) + '\n')
        self.output.flush()
        command = self.input.readline(8192)
        if not command:
            raise LabError('调试连接已关闭')
        request = json.loads(command)
        if request.get('action') not in ('step', 'continue'):
            raise LabError('无效的调试命令')
        points = request.get('breakpoints', [])
        if not isinstance(points, list) or len(points) > 64 or any(type(line) is not int or line < 1 for line in points):
            raise LabError('最多设置 64 个有效断点行号')
        self.breakpoints = set(points)
        self.mode = request['action']
        interpreter.started += time.monotonic() - paused_at


class Interpreter:
    def __init__(self, graph, steps=2000000, debugger=None):
        self.graph = graph
        self.limit = steps
        self.steps = 0
        self.allocated = 0
        self.line = 0
        self.depth = 0
        self.started = time.monotonic()
        self.trace = []
        self.trace_count = 0
        self.expansions = 0
        self.logs = []
        self.functions = {}
        self.debugger = debugger
        self.stack = []
        self.keep_current_route = False

    def tick(self, count=1):
        self.steps += count
        if self.steps > self.limit:
            raise LabError("执行步数达到上限")
        if time.monotonic() - self.started > 5:
            raise LabError("算法运行超过 5 秒")

    def allocate(self, count):
        self.allocated += count
        if count > MAX_CONTAINER or self.allocated > MAX_ALLOCATIONS:
            raise LabError("容器容量达到上限")

    def checked(self, value):
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if not math.isfinite(value) or abs(value) > 1e15:
                raise LabError("数值必须有限，绝对值不超过 10^15")
        if isinstance(value, str) and len(value) > 4096:
            raise LabError("字符串超过 4096 字符")
        return value

    def validate(self, source):
        if len(source.encode()) > 32768:
            raise LabError("代码超过 32 KiB")
        tree = ast.parse(source)
        for node in ast.walk(tree):
            if isinstance(node, (ast.Import, ast.ImportFrom, ast.ClassDef, ast.Lambda,
                                 ast.With, ast.AsyncFunctionDef, ast.Try, ast.Global,
                                 ast.Nonlocal, ast.ListComp, ast.SetComp, ast.DictComp,
                                 ast.GeneratorExp, ast.NamedExpr, ast.Yield, ast.Await)):
                self.line = node.lineno
                raise LabError("不支持 " + type(node).__name__ + "；请查看方法与语法说明")
            if isinstance(node, ast.Name) and node.id.startswith('_'):
                self.line = node.lineno
                raise LabError("不允许访问下划线开头的名称")
        for node in tree.body:
            self.line = node.lineno
            if not isinstance(node, ast.FunctionDef):
                raise LabError("顶层只允许函数定义，入口为 def route(ctx)")
            args = node.args
            if (node.decorator_list or args.defaults or args.kwonlyargs or args.vararg
                    or args.kwarg or args.posonlyargs or node.returns
                    or any(arg.annotation for arg in args.args)):
                raise LabError("函数仅支持普通位置参数")
            if node.name in self.functions or node.name.startswith('_'):
                raise LabError("重复或非法函数名")
            self.functions[node.name] = node
        if len(self.functions) > 16 or 'route' not in self.functions:
            raise LabError("需要 route(ctx) 入口，最多 16 个函数")
        if len(self.functions['route'].args.args) != 1:
            self.line = self.functions['route'].lineno
            raise LabError("route 入口必须恰好接收一个 ctx 参数")
        allowed_methods = {'start', 'goal', 'queue', 'estimate', 'neighbors', 'record', 'log', 'route', 'observation', 'keep', 'empty', 'push', 'pop', 'get', 'append', 'reverse'}
        allowed_calls = set(self.functions) | {'len', 'range', 'min', 'max', 'abs'}
        for node in ast.walk(tree):
            self.line = getattr(node, 'lineno', self.line)
            if isinstance(node, ast.Call):
                if node.keywords:
                    raise LabError("仅支持位置参数")
                if isinstance(node.func, ast.Name) and node.func.id not in allowed_calls:
                    raise LabError("不支持调用：" + node.func.id)
                if isinstance(node.func, ast.Attribute) and node.func.attr not in allowed_methods:
                    raise LabError("不支持的方法：" + node.func.attr)
        self.line = 0

    def run(self, source):
        self.validate(source)
        result = self.call_function('route', [self])
        if isinstance(result, KeepRoute):
            self.keep_current_route = True
            return None
        if result is not None and not isinstance(result, Route):
            raise LabError("请返回 ctx.route(states)，无路可达时返回 None")
        return result.states if result is not None else None

    def call_function(self, name, args):
        fn = self.functions[name]
        if len(fn.args.args) != len(args):
            raise LabError("函数参数数量不匹配：" + name)
        self.depth += 1
        if self.depth > 16:
            raise LabError("函数调用深度超过 16")
        scope = dict(zip((arg.arg for arg in fn.args.args), args))
        self.stack.append(name)
        try:
            self.block(fn.body, scope)
        except Returned as result:
            return result.value
        except (BreakLoop, ContinueLoop):
            raise LabError("break/continue 只能作用于当前函数内的循环")
        finally:
            self.depth -= 1
            self.stack.pop()
        return None

    def block(self, nodes, scope):
        for node in nodes:
            self.line = node.lineno
            if self.debugger:
                self.debugger.before(node, scope, self)
            self.tick()
            if isinstance(node, ast.Return):
                raise Returned(self.expr(node.value, scope) if node.value else None)
            elif isinstance(node, ast.Assign):
                value = self.expr(node.value, scope)
                for target in node.targets:
                    self.assign(target, value, scope)
            elif isinstance(node, ast.AugAssign):
                value = self.binary(node.op, self.expr(node.target, scope), self.expr(node.value, scope))
                self.assign(node.target, value, scope)
            elif isinstance(node, ast.Expr):
                self.expr(node.value, scope)
            elif isinstance(node, ast.If):
                self.block(node.body if self.expr(node.test, scope) else node.orelse, scope)
            elif isinstance(node, (ast.While, ast.For)):
                if node.orelse:
                    raise LabError("暂不支持循环 else")
                iterator = None
                if isinstance(node, ast.For):
                    values = self.expr(node.iter, scope)
                    if not isinstance(values, (list, tuple, dict, range)):
                        raise LabError("for 只能遍历容器或 range")
                    iterator = iter(values)
                first_iteration = True
                while True:
                    if self.debugger and not first_iteration:
                        self.debugger.before(node, scope, self)
                    first_iteration = False
                    self.tick()
                    if isinstance(node, ast.For):
                        try:
                            self.assign(node.target, next(iterator), scope)
                        except StopIteration:
                            break
                    elif not self.expr(node.test, scope):
                        break
                    try:
                        self.block(node.body, scope)
                    except BreakLoop:
                        break
                    except ContinueLoop:
                        continue
            elif isinstance(node, ast.Break):
                raise BreakLoop()
            elif isinstance(node, ast.Continue):
                raise ContinueLoop()
            elif not isinstance(node, ast.Pass):
                raise LabError("不支持语句：" + type(node).__name__)

    def assign(self, target, value, scope):
        if isinstance(target, ast.Name):
            scope[target.id] = value
        elif isinstance(target, (ast.Tuple, ast.List)):
            if not isinstance(value, (list, tuple)) or len(target.elts) != len(value):
                raise LabError("解包数量不匹配")
            for child, item in zip(target.elts, value):
                self.assign(child, item, scope)
        elif isinstance(target, ast.Subscript):
            container = self.expr(target.value, scope)
            key = self.expr(target.slice, scope)
            if not isinstance(container, (dict, list)):
                raise LabError("只允许修改自己的列表或字典")
            if isinstance(container, dict) and key not in container:
                if len(container) >= MAX_CONTAINER:
                    raise LabError("字典容量达到上限")
                self.allocate(1)
            container[key] = value
        else:
            raise LabError("不允许此赋值方式")

    def binary(self, op, left, right):
        if isinstance(op, ast.Add) and isinstance(left, (list, tuple)) and type(left) is type(right):
            self.allocate(len(left) + len(right))
            return left + right
        if isinstance(op, ast.Add) and isinstance(left, str) and isinstance(right, str):
            return self.checked(left + right)
        if not isinstance(left, (int, float)) or not isinstance(right, (int, float)):
            raise LabError("此运算仅支持数字")
        if isinstance(op, ast.Add): value = left + right
        elif isinstance(op, ast.Sub): value = left - right
        elif isinstance(op, ast.Mult): value = left * right
        elif isinstance(op, ast.Div): value = left / right
        elif isinstance(op, ast.FloorDiv): value = left // right
        elif isinstance(op, ast.Mod): value = left % right
        else: raise LabError("仅支持 + - * / // % 运算")
        return self.checked(value)

    def expr(self, node, scope):
        self.tick()
        self.line = getattr(node, 'lineno', self.line)
        if isinstance(node, ast.Constant):
            if not isinstance(node.value, (str, int, float, bool, type(None))):
                raise LabError("不支持此常量")
            return self.checked(node.value)
        if isinstance(node, ast.Name):
            if node.id not in scope: raise LabError("未定义变量：" + node.id)
            return scope[node.id]
        if isinstance(node, (ast.List, ast.Tuple)):
            self.allocate(len(node.elts))
            values = [self.expr(item, scope) for item in node.elts]
            return tuple(values) if isinstance(node, ast.Tuple) else values
        if isinstance(node, ast.Dict):
            self.allocate(len(node.keys))
            return {self.expr(k, scope): self.expr(v, scope) for k, v in zip(node.keys, node.values)}
        if isinstance(node, ast.Subscript):
            value = self.expr(node.value, scope)
            if not isinstance(value, (list, tuple, dict, str)):
                raise LabError("只允许读取容器索引")
            return value[self.expr(node.slice, scope)]
        if isinstance(node, ast.BinOp):
            return self.binary(node.op, self.expr(node.left, scope), self.expr(node.right, scope))
        if isinstance(node, ast.UnaryOp):
            value = self.expr(node.operand, scope)
            if isinstance(node.op, ast.Not): return not value
            if not isinstance(value, (int, float)): raise LabError("一元运算需要数字")
            if isinstance(node.op, ast.USub): return -value
            if isinstance(node.op, ast.UAdd): return value
        if isinstance(node, ast.BoolOp):
            value = self.expr(node.values[0], scope)
            for part in node.values[1:]:
                if isinstance(node.op, ast.And) and not value: return value
                if isinstance(node.op, ast.Or) and value: return value
                value = self.expr(part, scope)
            return value
        if isinstance(node, ast.Compare):
            left = self.expr(node.left, scope)
            for op, child in zip(node.ops, node.comparators):
                right = self.expr(child, scope)
                if isinstance(op, ast.Eq): ok = left == right
                elif isinstance(op, ast.NotEq): ok = left != right
                elif isinstance(op, ast.Lt): ok = left < right
                elif isinstance(op, ast.LtE): ok = left <= right
                elif isinstance(op, ast.Gt): ok = left > right
                elif isinstance(op, ast.GtE): ok = left >= right
                elif isinstance(op, ast.In): ok = left in right
                elif isinstance(op, ast.NotIn): ok = left not in right
                elif isinstance(op, ast.Is): ok = left is right
                elif isinstance(op, ast.IsNot): ok = left is not right
                else: raise LabError("不支持此比较")
                if not ok: return False
                left = right
            return True
        if isinstance(node, ast.Call):
            if node.keywords: raise LabError("仅支持位置参数")
            args = [self.expr(arg, scope) for arg in node.args]
            if isinstance(node.func, ast.Attribute):
                return self.method(self.expr(node.func.value, scope), node.func.attr, args)
            if isinstance(node.func, ast.Name):
                name = node.func.id
                if name in self.functions: return self.call_function(name, args)
                if name == 'len' and len(args) == 1 and isinstance(args[0], (list, tuple, dict, str, range)):
                    return len(args[0])
                if name == 'range' and 1 <= len(args) <= 3 and all(type(a) is int for a in args):
                    value = range(*args)
                    if len(value) > MAX_CONTAINER: raise LabError("range 长度达到上限")
                    return value
                if name in ('min', 'max', 'abs') and args and all(type(a) in (int, float) for a in args):
                    if name == 'abs' and len(args) == 1: return abs(args[0])
                    if name == 'min' and len(args) >= 2: return min(args)
                    if name == 'max' and len(args) >= 2: return max(args)
                raise LabError("不支持调用：" + name)
        raise LabError("不支持表达式：" + type(node).__name__)

    def state(self, value):
        if type(value) is not int or value < -2 or value >= len(self.graph['nodes']):
            raise LabError("无效的路由状态")
        return value

    def method(self, obj, name, args):
        if obj is self:
            if name == 'observation' and not args:
                return self.copy_observation(self.graph.get('observation', {'mode': 'static'}))
            if name == 'keep' and not args:
                if self.graph.get('observation', {}).get('mode') != 'vehicle':
                    raise LabError('ctx.keep() 仅用于车辆模式，静态实验请返回路线或 None')
                return KeepRoute()
            if name == 'start' and not args: return -1
            if name == 'goal' and not args: return -2
            if name == 'queue' and not args:
                self.allocate(1)
                return Queue()
            if name == 'estimate' and len(args) == 1:
                state = self.state(args[0])
                return self.graph['estimates'][state] if state >= 0 else 0
            if name == 'neighbors' and len(args) == 1:
                state = self.state(args[0])
                rows = self.graph['adjacency'].get(str(state), [])
                self.expansions += 1
                if self.expansions > 100000: raise LabError("后继查询超过 100000 次")
                self.tick(len(rows))
                self.allocate(len(rows) * 5)
                return [dict(state=r[0], edge=r[1], cost=r[2], length_m=r[3]) for r in rows]
            if name == 'record' and len(args) == 3:
                state = self.state(args[0])
                for v in args[1:]:
                    if type(v) not in (int, float): raise LabError("g、f 必须为有限数字")
                    self.checked(v)
                if state >= 0:
                    self.trace_count += 1
                    if len(self.trace) < 5000:
                        self.trace.append(dict(order=self.trace_count, nodeId=self.graph['nodes'][state], g=args[1], f=args[2]))
                return None
            if name == 'log' and len(args) == 1:
                value = args[0]
                if type(value) not in (str, int, float, bool, type(None)):
                    raise LabError("日志只支持数字、字符串、布尔值或 None")
                if len(self.logs) < 100: self.logs.append(str(value)[:1000])
                return None
            if name == 'route' and len(args) == 1 and isinstance(args[0], (list, tuple)):
                if not 2 <= len(args[0]) <= 10000: raise LabError("路线需要 2 至 10000 个状态")
                for state in args[0]: self.state(state)
                return Route(list(args[0]))
        if isinstance(obj, Queue):
            if name == 'empty' and not args: return not obj.items
            if name == 'push' and len(args) == 2:
                self.state(args[0])
                if type(args[1]) not in (int, float): raise LabError("队列优先级必须为数字")
                self.checked(args[1])
                self.allocate(1)
                if len(obj.items) >= MAX_CONTAINER: raise LabError("队列容量达到上限")
                heapq.heappush(obj.items, (args[1], obj.order, args[0]))
                obj.order += 1
                return None
            if name == 'pop' and not args:
                priority, _, state = heapq.heappop(obj.items)
                return (state, priority)
        if isinstance(obj, dict) and name == 'get' and 1 <= len(args) <= 2:
            return obj.get(*args)
        if isinstance(obj, list):
            if name == 'append' and len(args) == 1:
                self.allocate(1)
                if len(obj) >= MAX_CONTAINER: raise LabError("列表容量达到上限")
                obj.append(args[0])
                return None
            if name == 'reverse' and not args:
                self.tick(len(obj))
                obj.reverse()
                return None
        raise LabError("不支持的方法或参数数量：" + name)

    def copy_observation(self, value, depth=0):
        self.tick()
        if depth > 8: raise LabError('观察数据嵌套过深')
        if isinstance(value, dict):
            self.allocate(len(value) * 2)
            return {key: self.copy_observation(item, depth + 1) for key, item in value.items()}
        if isinstance(value, list):
            self.allocate(len(value))
            return [self.copy_observation(item, depth + 1) for item in value]
        return self.checked(value)


def execute(graph, source, steps=2000000, debugger=None):
    interpreter = Interpreter(graph, steps, debugger)
    try:
        states = interpreter.run(source)
        result = dict(ok=True, states=states, keepCurrentRoute=interpreter.keep_current_route)
    except Exception as error:
        message = str(error) if isinstance(error, (LabError, SyntaxError)) else {
            KeyError: '字典键不存在', IndexError: '索引越界或优先队列为空',
            ZeroDivisionError: '除数不能为零', TypeError: '操作数类型不匹配',
            RecursionError: '表达式嵌套过深', MemoryError: '内存达到上限',
            BreakLoop: 'break 只能在循环内使用', ContinueLoop: 'continue 只能在循环内使用',
        }.get(type(error), '不支持的操作：' + type(error).__name__)
        result = dict(ok=False, error=message, line=getattr(error, 'lineno', interpreter.line))
    result.update(logs=interpreter.logs, steps=interpreter.steps,
                  expandedNodes=interpreter.expansions,
                  computeMs=(time.monotonic() - interpreter.started) * 1000,
                  searchTrace=dict(stepCount=interpreter.trace_count,
                                   sampled=interpreter.trace_count > len(interpreter.trace), steps=interpreter.trace))
    return result


def check_source(source):
    interpreter = Interpreter({})
    try:
        interpreter.validate(source)
        return dict(ok=True)
    except Exception as error:
        return dict(ok=False, error=str(error) if isinstance(error, (LabError, SyntaxError)) else '不支持的语法',
                    line=getattr(error, 'lineno', interpreter.line) or 1)


if __name__ == '__main__':
    # Parent enforces a wall deadline and kills this single process on cancel.
    # User code cannot spawn children or access Python's runtime objects.
    resource.setrlimit(resource.RLIMIT_CPU, (8, 8))
    # Darwin does not implement RLIMIT_AS; the Go parent enforces RSS there.
    if sys.platform != "darwin":
        resource.setrlimit(resource.RLIMIT_AS, (1024 * 1024 * 1024,) * 2)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
    try:
        if sys.argv[1] == '--check':
            request = json.loads(sys.stdin.read(262144))
            print(json.dumps(check_source(request['source']), allow_nan=False))
            sys.exit(0)
        with open(sys.argv[1]) as context:
            graph = json.load(context)
        debugging = len(sys.argv) > 2 and sys.argv[2] == '--debug'
        request = json.loads(sys.stdin.readline(262144) if debugging else sys.stdin.read(262144))
        debugger = Debugger(sys.stdin, sys.stdout) if debugging else None
        result = execute(graph, request['source'], request.get('steps', 2000000), debugger)
        result['baseline'] = graph['baseline']
        print(json.dumps(dict(kind='finished', result=result) if debugging else result, allow_nan=False))
    except Exception as error:
        failed = dict(ok=False, error='执行环境初始化失败：' + type(error).__name__, line=0)
        print(json.dumps(dict(kind='finished', result=failed) if '--debug' in sys.argv[2:] else failed))
