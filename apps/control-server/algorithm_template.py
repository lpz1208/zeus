def route(ctx):
    frontier = ctx.queue()
    frontier.push(ctx.start(), 0)
    best = {ctx.start(): 0}
    parent = {}

    while not frontier.empty():
        state, cost = frontier.pop()
        if cost != best[state]:
            continue
        ctx.record(state, cost, cost)
        if state == ctx.goal():
            path = [state]
            while state != ctx.start():
                state = parent[state]
                path.append(state)
            path.reverse()
            ctx.log("已找到路线")
            return ctx.route(path)

        for step in ctx.neighbors(state):
            next_state = step["state"]
            next_cost = cost + step["cost"]
            if next_cost < best.get(next_state, 1e15):
                best[next_state] = next_cost
                parent[next_state] = state
                frontier.push(next_state, next_cost)
    return None
