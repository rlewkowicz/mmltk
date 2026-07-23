pub trait WasmNotSendSync: WasmNotSend + WasmNotSync {}
impl<T: WasmNotSend + WasmNotSync> WasmNotSendSync for T {}
pub trait WasmNotSend: Send {}
impl<T: Send> WasmNotSend for T {}
pub trait WasmNotSync: Sync {}
impl<T: Sync> WasmNotSync for T {}
