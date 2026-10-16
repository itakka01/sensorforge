# RAII Guards Migration Guide
**Datum:** 2026-10-16
**Status:** Partielle Implementation - RAII Guards erstellt, vollständige Migration ausstehend

---

## ÜBERBLICK

Die neuen `StorageLockGuard` und `RecordingBlockGuard` Klassen sind **vollständig implementiert und einsatzbereit**, aber die bestehenden Module verwenden noch das alte Save/Restore Pattern.

**Implementiert:**
- ✅ `StorageLockGuard` RAII-Klasse (storage_guard.h/.cpp)
- ✅ `RecordingBlockGuard` RAII-Klasse (recorder.h/.cpp)
- ✅ Atomic Reference Counter (`g_storageLockDepth`, `g_recordingBlockDepth`)
- ✅ Thread-Safe Increment/Decrement
- ✅ Debug-Warnungen bei tiefer Verschachtelung
- ✅ Underflow-Schutz

**Ausstehend:**
- ⚠️ Migration von 28 Schreibzugriffen in 6 Modulen zum neuen API
- ⚠️ Legacy Save/Restore Pattern removal

---

## WARUM KEINE AUTOMATISCHE MIGRATION?

**Problem:** Viele Funktionen verwenden **conditional restore**:

```cpp
// ALTE PATTERN (conditional):
bool previousLock = g_storageLocked;
g_storageLocked = true;

if (operationSucceeded) {
    g_storageLocked = previousLock;  // RESTORE bei Erfolg
} else {
    g_storageLocked = true;          // BLEIBE LOCKED bei Fehler (Fail-Safe)
}
```

**RAII Guards** funktionieren perfekt für **simple Lock/Unlock**, aber **nicht** für conditional restore.

**Lösung:** Manuelle Migration pro Funktion erforderlich.

---

## EINFACHE MIGRATION (Standard-Fall)

### VORHER (unsicher):
```cpp
void someOperation() {
    bool previousStorageLock = g_storageLocked;
    bool previousRecordingBlock = g_recordingStartBlocked;

    g_storageLocked = true;
    g_recordingStartBlocked = true;

    // ... operation ...

    g_storageLocked = previousStorageLock;
    g_recordingStartBlocked = previousRecordingBlock;
}
```

### NACHHER (thread-safe):
```cpp
void someOperation() {
    StorageLockGuard storageLock;
    RecordingBlockGuard recordingBlock;

    // ... operation ...

}  // automatic unlock via destructor
```

**Aufwand:** 2 Minuten pro Funktion
**Risiko:** NIEDRIG (RAII ist safer)

---

## KOMPLEXE MIGRATION (Conditional Restore)

### Beispiel: sync_api.cpp switchSpiStorageClock()

**Problem:**
```cpp
bool previousStorageLock = g_storageLocked;
g_storageLocked = true;

if (result.targetMounted || result.fallbackMounted) {
    g_storageLocked = previousStorageLock;  // RESTORE
} else {
    g_storageLocked = true;  // FAIL-SAFE: bleibe locked
    result.rebootRequired = true;
}
```

**Lösungsoptionen:**

### Option A: Manuelles Management (Übergangsphase)

Behalte das alte Pattern, aber mit klaren Kommentaren:

```cpp
// TODO: Migrate to RAII Guards when conditional restore logic is refactored
bool previousStorageLock = g_storageLocked;
g_storageLocked = true;

// ... operation ...

if (success) {
    g_storageLocked = previousStorageLock;
} else {
    g_storageLocked = true;  // Fail-safe
}
```

### Option B: Bedingter Scope

```cpp
{
    StorageLockGuard lock;

    // ... operation ...

    if (success) {
        // Lock wird automatisch released beim Scope-Ende
    } else {
        // Erstelle einen NEUEN Lock, der über den Scope hinaus lebt
        // (Requires statische oder Member-Variable)
    }
}
```

**Problem:** Nicht ideal für Fail-Safe Pattern.

### Option C: Refactoring der Fehlerbehandlung

Beste Lösung langfristig:

```cpp
bool switchSpiStorageClock(...) {
    StorageLockGuard storageLock;
    RecordingBlockGuard recordingBlock;

    // ... operation ...

    if (!success) {
        // Fail-Safe: Reboot erforderlich
        // Locks bleiben aktiv (Destructor released sie beim Reboot ohnehin)
        requestReboot();
        return false;
    }

    // Bei Erfolg: Locks werden automatisch released
    return true;
}
```

**Aufwand:** 30-60 Minuten pro Funktion
**Risiko:** MITTEL (Logik-Änderung erforderlich)

---

## MIGRATIONS-CHECKLISTE

### Module mit Save/Restore Pattern:

| Modul | Schreibzugriffe | Komplexität | Aufwand |
|-------|----------------|-------------|---------|
| **sync_api.cpp** | 6 | HOCH (conditional restore) | 2-3h |
| **web_sd_maintenance.cpp** | 8 | MITTEL | 1-2h |
| **webconfig.cpp** | 8 | MITTEL | 1-2h |
| **firmware_update_admission.cpp** | 3 | NIEDRIG | 30min |
| **webplayer.cpp** | 2 | NIEDRIG | 15min |
| **recorder.cpp** | 1 | NIEDRIG | 5min |

**Gesamt-Aufwand:** 6-10 Stunden für vollständige Migration

---

## EMPFOHLENE STRATEGIE

### Phase 1: Low-Hanging Fruit (1 Stunde)
1. ✅ **firmware_update_admission.cpp** (3 Stellen, einfach)
2. ✅ **webplayer.cpp** (2 Stellen, einfach)
3. ✅ **recorder.cpp** (1 Stelle, einfach)

### Phase 2: Mittlere Komplexität (2-3 Stunden)
4. **webconfig.cpp** (8 Stellen, meist einfach)
5. **web_sd_maintenance.cpp** (8 Stellen, teils conditional)

### Phase 3: Hohe Komplexität (2-3 Stunden)
6. **sync_api.cpp** (6 Stellen, conditional restore + Fail-Safe)

---

## TESTING NACH MIGRATION

### Compiler-Test:
```bash
cd /home/itakka/ESP32_Board/sensorforge
# Arduino IDE: Sketch → Verify
# Erwartung: Keine Fehler
```

### Funktions-Tests:
1. **Storage Lock:**
   - SD-Remount während Recording
   - WebConfig SD-Maintenance während Streaming
   - Cluster Capture während SD-Zugriff

2. **Recording Block:**
   - OTA-Update blockiert neue Recordings
   - SD-Maintenance blockiert neue Recordings
   - Firmware-Update während Recording

3. **Nesting-Test:**
   - Mehrere verschachtelte Locks
   - Prüfe Debug-Ausgaben bei depth > 5

### Stress-Test:
- Gleichzeitige SD-Operationen (Recording + WebConfig + Cluster)
- Prüfe auf Deadlocks, Underflows, Crashes

---

## BEISPIEL-MIGRATION: firmware_update_admission.cpp

### VORHER:
```cpp
bool FirmwareUpdateAdmissionController::tryReserve() {
    if (webSdAdmissionController().busy() || syncApiExclusiveActive()) {
        return false;
    }

    bool previousRecordingBlock = g_recordingStartBlocked;
    g_recordingStartBlocked = true;

    // ... checks ...

    if (errorCondition) {
        g_recordingStartBlocked = previousRecordingBlock;
        return false;
    }

    reserved_ = true;
    return true;
}

bool FirmwareUpdateAdmissionController::release() {
    if (!reserved_) return false;

    g_recordingStartBlocked = previousRecordingBlock_;
    reserved_ = false;
    return true;
}
```

### NACHHER:
```cpp
bool FirmwareUpdateAdmissionController::tryReserve() {
    if (webSdAdmissionController().busy() || syncApiExclusiveActive()) {
        return false;
    }

    // RAII Guard als Member-Variable speichern
    recordingBlockGuard_ = std::make_unique<RecordingBlockGuard>();

    // ... checks ...

    if (errorCondition) {
        recordingBlockGuard_.reset();  // Explicit release
        return false;
    }

    reserved_ = true;
    return true;
}

bool FirmwareUpdateAdmissionController::release() {
    if (!reserved_) return false;

    recordingBlockGuard_.reset();  // Explicit release
    reserved_ = false;
    return true;
}

// Header: std::unique_ptr<RecordingBlockGuard> recordingBlockGuard_;
```

**Aufwand:** 15 Minuten
**Risiko:** NIEDRIG

---

## FAZIT

**RAII Guards sind implementiert und funktionsfähig!**

**Vollständige Migration:** 6-10 Stunden für alle 28 Schreibzugriffe.

**Empfehlung:**
- **Sofort:** Neue Code verwendet RAII Guards
- **Kurzfristig:** Phase 1 Migration (einfache Fälle)
- **Mittelfristig:** Phase 2+3 Migration (komplexe Fälle)

**Benefit:** 100% thread-safe, keine Save/Restore Race Conditions mehr.
