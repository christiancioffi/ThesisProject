class TimeoutException(Exception):
    def __init__(self, message="Timeout"):
        super().__init__(message)