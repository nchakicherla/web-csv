// device.js - one shared WebGPU device for the whole page.
//
// requestAdapter/requestDevice are both async and should only run once;
// every caller (today just bridge.js) awaits the same promise instead of
// racing separate adapter requests.

let devicePromise = null;

export function getDevice() {
	if (!devicePromise) {
		devicePromise = (async () => {
			if (!navigator.gpu) {
				throw new Error('WebGPU is not available in this browser');
			}
			const adapter = await navigator.gpu.requestAdapter();
			if (!adapter) {
				throw new Error('WebGPU is available but no adapter was returned');
			}
			return adapter.requestDevice();
		})();
	}
	return devicePromise;
}
