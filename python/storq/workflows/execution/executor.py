import asyncio
import logging
import time
from datetime import datetime, timezone

from ..execution.dependency import DependencyResolver
from ..execution.retry import RetryManager
from ..execution.state import StepStatus, WorkflowStatus
from ..events.event import WorkflowEvent
from ..results.result import StepResult, WorkflowResult

logger = logging.getLogger("storq.workflows")


class WorkflowExecutor:
    async def execute(self, workflow) -> WorkflowResult:
        workflow.validate()
        workflow.state_manager.transition(WorkflowStatus.VALIDATING)
        workflow.state_manager.transition(WorkflowStatus.READY)
        workflow.state_manager.transition(WorkflowStatus.RUNNING)
        await workflow.events.dispatch(WorkflowEvent("workflow_started", workflow.id))
        resolver = DependencyResolver(workflow.steps)
        completed, running = set(), set()
        failed, skipped, terminal = set(), set(), set()
        semaphore = asyncio.Semaphore(workflow.config.max_concurrency)
        pending = {step.name: step for step in workflow.steps}

        async def run_step(step):
            nonlocal completed, failed, skipped
            async with semaphore:
                await workflow.events.dispatch(WorkflowEvent("step_started", workflow.id, step.name))
                if step.state.status == StepStatus.PENDING:
                    step.state.transition(StepStatus.READY)
                step.state.transition(StepStatus.RUNNING)
                step.state.attempts += 1
                started = datetime.now(timezone.utc)
                monotonic = time.monotonic()
                try:
                    operation = step.execute(workflow.context)
                    if step.timeout is not None:
                        output = await asyncio.wait_for(operation, step.timeout)
                    else:
                        output = await operation
                    step.output = output
                    workflow.context.set_output(step.name, output)
                    step.state.transition(StepStatus.COMPLETED)
                    completed.add(step.name)
                    workflow.step_results[step.name] = StepResult(
                        step.id, step.name, "completed", output, attempts=step.state.attempts,
                        started_at=started, completed_at=datetime.now(timezone.utc),
                        duration=time.monotonic() - monotonic)
                    await workflow.events.dispatch(WorkflowEvent(
                        "step_completed", workflow.id, step.name,
                        duration=time.monotonic() - monotonic))
                except asyncio.CancelledError:
                    if workflow.cancellation.requested:
                        step.state.transition(StepStatus.CANCELLED)
                        workflow.cancellation.mark_uncancellable(step.name)
                    else:
                        step.state.transition(StepStatus.CANCELLED)
                    raise
                except BaseException as error:
                    attempts = step.state.attempts
                    while (attempts < step.retry.max_attempts
                           and RetryManager.should_retry(step.retry, error, attempts)
                           and step.idempotent):
                        step.state.transition(StepStatus.RETRYING)
                        await asyncio.sleep(step.retry.delay_for_attempt(attempts))
                        await workflow.events.dispatch(WorkflowEvent(
                            "step_retrying", workflow.id, step.name,
                            metadata={"attempt": attempts + 1}))
                        step.state.transition(StepStatus.RUNNING)
                        try:
                            operation = step.execute(workflow.context)
                            output = await asyncio.wait_for(operation, step.timeout) if step.timeout else await operation
                            step.output = output
                            workflow.context.set_output(step.name, output)
                            step.state.transition(StepStatus.COMPLETED)
                            completed.add(step.name)
                            workflow.step_results[step.name] = StepResult(
                                step.id, step.name, "completed", output, attempts=step.state.attempts + 1,
                                started_at=started, completed_at=datetime.now(timezone.utc),
                                duration=time.monotonic() - monotonic)
                            break
                        except BaseException as retry_error:
                            error = retry_error
                            attempts += 1
                            step.state.attempts = attempts
                    if step.name in completed:
                        return
                    step.state.transition(StepStatus.FAILED)
                    step.error = error
                    step.state.error = str(error)
                    failed.add(step.name)
                    workflow.step_results[step.name] = StepResult(
                        step.id, step.name, "failed", error=error,
                        attempts=attempts, started_at=started,
                        completed_at=datetime.now(timezone.utc),
                        duration=time.monotonic() - monotonic)
                    await workflow.events.dispatch(WorkflowEvent(
                        "step_failed", workflow.id, step.name,
                        duration=time.monotonic() - monotonic,
                        metadata={"error": str(error)}))

        tasks = set()
        while pending or tasks:
            if workflow.cancellation.requested:
                break
            if workflow.config.deterministic and tasks:
                done = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                tasks = done[1] if isinstance(done, tuple) else tasks
            ready = resolver.ready(completed, running)
            ready = [pending[name] for name in ready if name in pending]
            for step in ready[:workflow.config.max_concurrency]:
                pending.pop(step.name, None)
                running.add(step.name)
                tasks.add(asyncio.create_task(run_step(step)))

            if tasks:
                done, tasks = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass
                    else:
                        name = next((item for item in pending if False), None)
                running.clear()
                running.update({name for name in pending if any(t for t in [])})
            elif ready:
                continue
            else:
                break

            for name, step in list(pending.items()):
                if not all(dep in (completed | failed) for dep in step.dependencies):
                    continue
                if any(dep in failed for dep in step.dependencies) and name not in terminal:
                    step.state.transition(StepStatus.SKIPPED)
                    skipped.add(name)
                    terminal.add(name)
                    pending.pop(name, None)

        # Independent failures can be observed before scheduling their dependants.
        for name, step in list(pending.items()):
            if any(dep in failed for dep in step.dependencies):
                step.state.transition(StepStatus.SKIPPED)
                skipped.add(name)
                workflow.step_results[name] = StepResult(step.id, name, "skipped")
                pending.pop(name, None)

        if workflow.cancellation.requested:
            for task in tasks:
                task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            workflow.state_manager.transition(WorkflowStatus.CANCELLING)
            workflow.state_manager.transition(WorkflowStatus.CANCELLED)
            status = WorkflowStatus.CANCELLED.value
        elif failed:
            status = WorkflowStatus.FAILED.value
            workflow.state_manager.transition(WorkflowStatus.FAILED)
        elif skipped:
            status = WorkflowStatus.PARTIALLY_COMPLETED.value
            workflow.state_manager.transition(WorkflowStatus.PARTIALLY_COMPLETED)
        else:
            status = WorkflowStatus.COMPLETED.value
            workflow.state_manager.transition(WorkflowStatus.COMPLETED)

        now = datetime.now(timezone.utc)
        result = WorkflowResult(
            workflow.id, status, workflow._started_at or now, now,
            step_results=workflow.step_results,
            failed_steps=sorted(failed), skipped_steps=sorted(skipped),
            errors=[workflow.step_results[name].error for name in failed
                    if workflow.step_results[name].error is not None],
            outputs=workflow.context.outputs,
        )
        await workflow.events.dispatch(WorkflowEvent("workflow_completed" if not failed and not skipped else "workflow_failed",
                                                     workflow.id, duration=result.duration))
        workflow.history.append(result)
        workflow._result = result
        return result
