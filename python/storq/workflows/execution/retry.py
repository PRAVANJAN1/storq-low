from dataclasses import dataclass
from ..steps.base import RetryPolicy


class RetryManager:
    @staticmethod
    def should_retry(policy: RetryPolicy, error: BaseException, attempt: int) -> bool:
        if attempt >= policy.max_attempts:
            return False
        if isinstance(error, policy.non_retryable_exceptions):
            return False
        return isinstance(error, policy.retryable_exceptions)
