/**
 * 中国移动 OneNET 开放平台 API 封装
 *
 * 平台参数来自 P1 工程 Hardware/MQTT.h：
 *   产品 ID   : q8oyFaE9YD
 *   设备名称  : HJ_Family
 *   设备 Token: version=2018-10-31&res=products%2Fq8oyFaE9YD%2Fdevices%2FHJ_Family&et=1858232738&method=md5&sign=Zq4a3JgO7R5oq%2BMzuX44yw%3D%3D
 *
 * 物模型属性：
 *   relay1 ~ relay4 : bool  继电器开关
 *   temp / humi     : float 温度 / 湿度
 */

const CONFIG = {
	baseUrl: 'https://iot-api.heclouds.com',
	productId: 'q8oyFaE9YD',
	deviceName: 'HJ_Family',
	authorization: 'version=2018-10-31&res=products%2Fq8oyFaE9YD%2Fdevices%2FHJ_Family&et=1858232738&method=md5&sign=Zq4a3JgO7R5oq%2BMzuX44yw%3D%3D'
}

function buildQuery(params) {
	const keys = Object.keys(params)
	const parts = []
	for (let i = 0; i < keys.length; i++) {
		parts.push(keys[i] + '=' + encodeURIComponent(params[keys[i]]))
	}
	return parts.join('&')
}

/**
 * 通用请求，返回 Promise<data 字段>
 * OneNET 约定：code === 0 成功，其余为错误码
 */
function request(method, path, params, body) {
	return new Promise((resolve, reject) => {
		let url = CONFIG.baseUrl + path
		if (params) url += '?' + buildQuery(params)
		uni.request({
			url: url,
			method: method || 'GET',
			data: body || {},
			header: {
				'authorization': CONFIG.authorization
			},
			success: res => {
				const d = res.data
				if (d && d.code === 0) {
					resolve(d.data)
				} else {
					reject(new Error((d && d.msg) || ('请求失败(' + res.statusCode + ')')))
				}
			},
			fail: err => {
				reject(new Error((err && err.errMsg) || '网络请求失败'))
			}
		})
	})
}

/**
 * 查询设备所有属性最新值
 * 返回数组: [{ identifier, time, value, name, ... }]
 */
function getLatestProperties() {
	return request('GET', '/thingmodel/query-device-property', {
		product_id: CONFIG.productId,
		device_name: CONFIG.deviceName
	})
}

/**
 * 设置设备属性(下发控制指令)
 * params 形如 { relay1: true }
 */
function setProperty(params) {
	return request('POST', '/thingmodel/set-device-property', null, {
		product_id: CONFIG.productId,
		device_name: CONFIG.deviceName,
		params: params
	})
}

/**
 * 查询设备状态
 * 返回 { online: bool, lastTime: string }
 */
async function getDeviceStatus() {
	const data = await request('GET', '/device/detail', {
		product_id: CONFIG.productId,
		device_name: CONFIG.deviceName
	})
	return {
		online: !!(data && data.status === 1),
		lastTime: (data && data.last_time) || ''
	}
}

/**
 * 查询属性历史数据(自动分页, 返回按时间升序的 [{time, value}] )
 * @param identifier 属性标识, 如 'temp' / 'humi'
 * @param startMs   起始时间(毫秒时间戳)
 * @param endMs     结束时间(毫秒时间戳)
 * @param onProgress 可选, 每取回一页回调(已累计点数)
 */
async function queryHistory(identifier, startMs, endMs, onProgress) {
	const PAGE = 1000        // 单页最多 1000 条
	const MAX_POINTS = 8000  // 上限保护, 防止请求过多
	let offset = 0
	let all = []

	while (true) {
		const data = await request('GET', '/thingmodel/query-device-property-history', {
			product_id: CONFIG.productId,
			device_name: CONFIG.deviceName,
			identifier: identifier,
			start_time: startMs,
			end_time: endMs,
			sort: 2, // 倒序(最新在前), 分页取回后再排序
			limit: PAGE,
			offset: offset
		})

		// 返回格式兼容: data 可能是数组, 也可能是 { list: [...] }
		let list = []
		if (Array.isArray(data)) {
			list = data
		} else if (data && Array.isArray(data.list)) {
			list = data.list
		}

		all = all.concat(list)
		if (onProgress) onProgress(all.length)

		if (list.length < PAGE || all.length >= MAX_POINTS) break
		offset += PAGE
	}

	// time 转数字, value 转浮点, 按时间升序
	const points = []
	for (let i = 0; i < all.length; i++) {
		const v = parseFloat(all[i].value)
		const t = Number(all[i].time)
		if (!isNaN(v) && !isNaN(t)) {
			points.push({ time: t, value: v })
		}
	}
	points.sort((a, b) => a.time - b.time)
	return points
}

export {
	CONFIG,
	getLatestProperties,
	setProperty,
	getDeviceStatus,
	queryHistory
}
