-- =====================================================================================
-- TERRAPASS v2.0 - ESQUEMA POSTGRESQL + POSTGIS + REALTIME (SUPABASE)
-- =====================================================================================

CREATE EXTENSION IF NOT EXISTS postgis;

-- 1. Areas de Interes (AOI)
CREATE TABLE IF NOT EXISTS aois (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    description TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 2. Celdas geograficas de la cuadricula (~100m)
CREATE TABLE IF NOT EXISTS grid_cells (
    id BIGSERIAL PRIMARY KEY,
    aoi_id TEXT REFERENCES aois(id) ON DELETE CASCADE,
    row_idx INT NOT NULL,
    col_idx INT NOT NULL,
    center_point GEOMETRY(Point, 4326),
    UNIQUE(aoi_id, row_idx, col_idx)
);

-- 3. Series temporales de telemetria de radar (NISAR Banda L: GCOV / GUNW)
CREATE TABLE IF NOT EXISTS radar_series (
    cell_id BIGINT REFERENCES grid_cells(id) ON DELETE CASCADE,
    pass_index INT NOT NULL,
    observation_date DATE NOT NULL,
    hh_db REAL NOT NULL,
    hv_db REAL NOT NULL,
    coherence REAL NOT NULL,
    displacement_mm REAL DEFAULT 0.0,
    PRIMARY KEY (cell_id, pass_index)
);

-- 4. Eventos clasificados (Pasaporte de Cambio)
CREATE TABLE IF NOT EXISTS passport_events (
    id BIGSERIAL PRIMARY KEY,
    cell_id BIGINT REFERENCES grid_cells(id) ON DELETE CASCADE,
    event_type TEXT CHECK (event_type IN ('deformation', 'flood_open', 'flood_veg', 'veg_loss', 'ag_activity', 'unknown')),
    start_pass INT NOT NULL,
    end_pass INT NOT NULL,
    confidence_score NUMERIC(4, 3) NOT NULL,
    confidence_level TEXT CHECK (confidence_level IN ('alta', 'media', 'baja')),
    human_explanation TEXT NOT NULL,
    technical_signal TEXT NOT NULL,
    calval_verified BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 5. Telemetria Multi-Sensor de Estaciones IoT en Tierra (Cal/Val Nodes)
CREATE TABLE IF NOT EXISTS ground_truth (
    id BIGSERIAL PRIMARY KEY,
    node_id TEXT NOT NULL,
    aoi_id TEXT REFERENCES aois(id) ON DELETE CASCADE,
    cell_id BIGINT,
    timestamp TIMESTAMPTZ DEFAULT NOW(),
    soil_moisture_pct NUMERIC(5, 2) NOT NULL,
    water_level_cm NUMERIC(5, 1) DEFAULT 0.0,
    dielectric_eps NUMERIC(5, 1) DEFAULT 4.0,
    ambient_temp_c NUMERIC(4, 2) DEFAULT 25.0,
    air_humidity_pct NUMERIC(5, 2) DEFAULT 65.0,
    vibration_g NUMERIC(5, 3) DEFAULT 0.000,
    tilt_deg NUMERIC(5, 2) DEFAULT 0.00,
    local_regime TEXT DEFAULT 'ESTABLE_NOMINAL'
);

-- Semillas iniciales para las 3 Zonas de Estudio (AOIs)
INSERT INTO aois (id, name, latitude, longitude, description) VALUES
('ven', 'Venezuela · Sismo y Cuenca', 10.2, -69.5, 'Monitoreo interferometrico GUNW de deformacion cortical y desborde fluvial.'),
('per', 'Peru · Arrozales y Bosque', -6.4, -76.4, 'Monitoreo polarimetrico GCOV de doble rebote en bosque inundable y parcelas de arroz.'),
('ctl', 'Atacama · Control Estable', -24.1, -69.9, 'Zona hiperarida de control estadistico para verificacion de tasa cero de falsos positivos.')
ON CONFLICT (id) DO NOTHING;

-- Políticas RLS para permitir insercion desde el ESP32 (anon key) y lectura desde la web
ALTER TABLE ground_truth ENABLE ROW LEVEL SECURITY;
CREATE POLICY "Permitir lectura publica de ground_truth" ON ground_truth FOR SELECT USING (true);
CREATE POLICY "Permitir insercion IoT en ground_truth" ON ground_truth FOR INSERT WITH CHECK (true);

-- Habilitar publicacion en Supabase Realtime
ALTER PUBLICATION supabase_realtime ADD TABLE ground_truth;
ALTER PUBLICATION supabase_realtime ADD TABLE passport_events;

CREATE INDEX IF NOT EXISTS idx_ground_truth_aoi_time ON ground_truth(aoi_id, timestamp DESC);
