CREATE TABLE IF NOT EXISTS AudioChunks (
    ID SERIAL PRIMARY KEY,
    filename VARCHAR(100) NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL, 
    battery_level INT NOT NULL,
    node_id VARCHAR(100) NOT NULL,
    rms FLOAT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_timestamp ON AudioChunks (timestamp);

CREATE TABLE IF NOT EXISTS Events (
    ID SERIAL PRIMARY KEY,
    event_type VARCHAR(100) NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL
);

CREATE TABLE IF NOT EXISTS Predictions (
    ID SERIAL PRIMARY KEY,
    timestamp TIMESTAMPTZ NOT NULL,
    prediction FLOAT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_predictions_timestamp ON Predictions (timestamp);
