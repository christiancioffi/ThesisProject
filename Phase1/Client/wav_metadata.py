class WAVMetadata:
    def __init__(self, tmst: str, noId: str, blvl: int, rmsv: float):
        self.tmst = tmst
        self.noId = noId
        self.blvl = blvl
        self.rmsv = rmsv

    def to_dict(self) -> dict:
        return {
            "tmst": self.tmst,
            "noId": self.noId,
            "blvl": self.blvl,
            "rmsv": self.rmsv
        }

