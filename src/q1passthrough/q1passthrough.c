// q1passthrough: camera passthrough test scene for the native Quest 1.
//
// Asks Monado's quest1 driver for the tracking camera frames (heartbeat in quest1_cameras_shm.h;
// the driver streams only while someone asks), stitches the four fisheye cameras into one equirectangular panorama
// with the factory calibration, and shows it as an XR_KHR_composition_layer_equirect2 layer,
// placed at the head orientation of the capture time (so it stays put while the head turns).
// A 25 cm cube made of quad layers sits 1 m in front of the start position.
// Mono passthrough at infinite distance: near objects show the camera parallax.
//
// Run as root while monado-service runs: xr-run q1passthrough
// Build: see build.sh.
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "quest1_cameras_shm.h"

#define PANO_W 1536
#define PANO_H 768
#define FACE_PX 64
#define CUBE_SIZE 0.25f
#define MAX_FOV_RAD 1.48f // ~85 deg from the optical axis

#define LOG(...) fprintf(stderr, "q1passthrough: " __VA_ARGS__)
#define XR(call)                                                                                                       \
	do {                                                                                                           \
		XrResult r_ = (call);                                                                                  \
		if (XR_FAILED(r_)) {                                                                                   \
			LOG("%s failed: %d (line %d)\n", #call, r_, __LINE__);                                         \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)
#define VK(call)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (call);                                                                                  \
		if (r_ != VK_SUCCESS) {                                                                                \
			LOG("%s failed: %d (line %d)\n", #call, r_, __LINE__);                                         \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static volatile sig_atomic_t quit_requested;
static void
on_signal(int sig)
{
	(void)sig;
	quit_requested = 1;
}


/*
 * Quaternion helpers (x, y, z, w).
 */

static XrQuaternionf
quat_mul(XrQuaternionf a, XrQuaternionf b)
{
	return (XrQuaternionf){
	    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

static XrQuaternionf
quat_axis_angle(float x, float y, float z, float angle)
{
	float s = sinf(angle / 2);
	return (XrQuaternionf){x * s, y * s, z * s, cosf(angle / 2)};
}

static XrVector3f
quat_rotate(XrQuaternionf q, XrVector3f v)
{
	// v + 2w(u x v) + 2u x (u x v)
	XrVector3f u = {q.x, q.y, q.z};
	XrVector3f t = {2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
	return (XrVector3f){
	    v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
	    v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
	    v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
	};
}


/*
 * Camera frames and the panorama lookup table.
 */

static int64_t
monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

//! Keeps the driver's cameras running (it stops them 2 s after the last heartbeat).
static void
heartbeat(struct q1cam_shm *shm)
{
	__atomic_store_n(&shm->demand_ns, (uint64_t)monotonic_ns(), __ATOMIC_RELEASE);
}

static struct q1cam_shm *
open_cameras(void)
{
	for (int tries = 0;; tries++) {
		int fd = shm_open(Q1CAM_SHM_NAME, O_RDWR, 0);
		if (fd >= 0) {
			void *p = mmap(NULL, sizeof(struct q1cam_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			close(fd);
			struct q1cam_shm *shm = p == MAP_FAILED ? NULL : p;
			if (shm != NULL && __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) == Q1CAM_MAGIC &&
			    shm->version == Q1CAM_VERSION) {
				heartbeat(shm);
				return shm;
			}
			if (shm != NULL) {
				munmap(p, sizeof(struct q1cam_shm));
			}
		}
		if (tries == 0) {
			LOG("waiting for %s (monado-service running? QUEST1_CAMERAS=0?)\n", Q1CAM_SHM_NAME);
		}
		if (quit_requested) {
			exit(1);
		}
		usleep(200 * 1000);
	}
}

//! Device-frame direction -> pixel of camera k; false when outside the image.
static bool
project(const struct q1cam_calib *k, const float d[3], float *px, float *py, float *theta)
{
	const float *M = k->device_from_camera; // camera = R^T * device
	float x = M[0] * d[0] + M[4] * d[1] + M[8] * d[2];
	float y = M[1] * d[0] + M[5] * d[1] + M[9] * d[2];
	float z = M[2] * d[0] + M[6] * d[1] + M[10] * d[2];
	float r = sqrtf(x * x + y * y);
	float t = atan2f(r, z);
	if (t > MAX_FOV_RAD || r < 1e-9f) {
		return false;
	}
	float t2 = t * t, poly = 1, tp = 1;
	for (int i = 0; i < 6; i++) {
		tp *= t2;
		poly += k->k[i] * tp;
	}
	float ux = t * poly * x / r, uy = t * poly * y / r;
	float r2 = ux * ux + uy * uy, tmp = 2 * (ux * k->p[0] + uy * k->p[1]);
	*px = k->f * (ux + tmp * ux + r2 * k->p[0]) + k->cx;
	*py = k->f * (uy + tmp * uy + r2 * k->p[1]) + k->cy;
	*theta = t;
	return *px >= 0 && *px < Q1CAM_W - 1 && *py >= 0 && *py < Q1CAM_H - 1;
}

//! Per panorama pixel: camera index << 24 | pixel offset, or UINT32_MAX for no camera.
static uint32_t *
build_lut(const struct q1cam_shm *shm)
{
	uint32_t *lut = malloc(sizeof(uint32_t) * PANO_W * PANO_H);
	int covered = 0;
	for (int v = 0; v < PANO_H; v++) {
		float th = ((float)v + 0.5f) / PANO_H * (float)M_PI;
		for (int u = 0; u < PANO_W; u++) {
			// Inverse of Monado's layer_equirect2.frag: lon = atan(x, -z), lat = acos(y).
			float lam = (((float)u + 0.5f) / PANO_W - 0.5f) * 2 * (float)M_PI;
			float d[3] = {sinf(lam) * sinf(th), cosf(th), -cosf(lam) * sinf(th)};
			uint32_t best = UINT32_MAX;
			float best_t = 1e9f;
			for (int c = 0; c < Q1CAM_N; c++) {
				float px, py, t;
				if ((shm->calib_valid & (1u << c)) && project(&shm->calib[c], d, &px, &py, &t) &&
				    t < best_t) {
					best_t = t;
					best = (uint32_t)c << 24 | (uint32_t)((int)(py + 0.5f) * Q1CAM_W + (int)(px + 0.5f));
				}
			}
			lut[v * PANO_W + u] = best;
			covered += best != UINT32_MAX;
		}
	}
	LOG("panorama %dx%d, %.0f %% covered by the cameras\n", PANO_W, PANO_H, 100.0 * covered / (PANO_W * PANO_H));
	return lut;
}

static void
remap(const uint32_t *lut, const struct q1cam_slot *s, uint32_t *out)
{
	for (int i = 0; i < PANO_W * PANO_H; i++) {
		uint32_t e = lut[i];
		uint32_t g = e == UINT32_MAX ? 0 : s->pixels[e >> 24][e & 0xffffff];
		out[i] = 0xff000000u | g << 16 | g << 8 | g;
	}
}


/*
 * Vulkan helpers.
 */

struct gpu
{
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	void *staging_ptr;
};

static void
create_staging(struct gpu *g, VkDeviceSize size)
{
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = size,
	                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VK(vkCreateBuffer(g->device, &bci, NULL, &g->staging));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g->device, g->staging, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(g->phys, &mp);
	uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		LOG("no host-visible coherent memory\n");
		exit(1);
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
	VK(vkAllocateMemory(g->device, &mai, NULL, &g->staging_mem));
	VK(vkBindBufferMemory(g->device, g->staging, g->staging_mem, 0));
	VK(vkMapMemory(g->device, g->staging_mem, 0, VK_WHOLE_SIZE, 0, &g->staging_ptr));
}

static void
barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	                          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
	                          .oldLayout = from,
	                          .newLayout = to,
	                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .image = image,
	                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
	                     NULL, 1, &b);
}

//! Copy the staging buffer (tightly packed RGBA, w x h) into a swapchain image.
static void
upload(struct gpu *g, VkImage image, uint32_t w, uint32_t h)
{
	VK(vkResetCommandBuffer(g->cmd, 0));
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VK(vkBeginCommandBuffer(g->cmd, &bi));
	barrier(g->cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
	vkCmdCopyBufferToImage(g->cmd, g->staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier(g->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VK(vkEndCommandBuffer(g->cmd));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g->cmd};
	VK(vkResetFences(g->device, 1, &g->fence));
	VK(vkQueueSubmit(g->queue, 1, &si, g->fence));
	VK(vkWaitForFences(g->device, 1, &g->fence, VK_TRUE, UINT64_MAX));
}


/*
 * OpenXR setup.
 */

struct app
{
	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace local, view;
	struct gpu gpu;
	int64_t format;
	bool bgra;
	XrSwapchain pano, faces;
	VkImage pano_images[8], face_image;
	uint32_t pano_count;
	PFN_xrConvertTimespecTimeToTimeKHR convert_time;
};

#define XR_PROC(app, name, var) XR(xrGetInstanceProcAddr(app->instance, #name, (PFN_xrVoidFunction *)&var))

static void
init_xr(struct app *a)
{
	const char *exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME,
	                      XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
	                            .enabledExtensionCount = 3,
	                            .enabledExtensionNames = exts,
	                            .applicationInfo = {.applicationName = "q1passthrough",
	                                                .applicationVersion = 1,
	                                                .apiVersion = XR_MAKE_VERSION(1, 0, 34)}};
	XR(xrCreateInstance(&ici, &a->instance));
	XR_PROC(a, xrConvertTimespecTimeToTimeKHR, a->convert_time);
	XrSystemGetInfo sgi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XR(xrGetSystem(a->instance, &sgi, &a->system));

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs;
	PFN_xrCreateVulkanInstanceKHR create_instance;
	PFN_xrGetVulkanGraphicsDevice2KHR get_device;
	PFN_xrCreateVulkanDeviceKHR create_device;
	XR_PROC(a, xrGetVulkanGraphicsRequirements2KHR, get_reqs);
	XR_PROC(a, xrCreateVulkanInstanceKHR, create_instance);
	XR_PROC(a, xrGetVulkanGraphicsDevice2KHR, get_device);
	XR_PROC(a, xrCreateVulkanDeviceKHR, create_device);
	XrGraphicsRequirementsVulkan2KHR reqs = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	XR(get_reqs(a->instance, a->system, &reqs));

	struct gpu *g = &a->gpu;
	VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                              .pApplicationName = "q1passthrough",
	                              .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo vici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info};
	XrVulkanInstanceCreateInfoKHR xvici = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
	                                       .systemId = a->system,
	                                       .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	                                       .vulkanCreateInfo = &vici};
	VkResult vr;
	XR(create_instance(a->instance, &xvici, &g->instance, &vr));
	VK(vr);
	XrVulkanGraphicsDeviceGetInfoKHR dgi = {
	    .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = a->system, .vulkanInstance = g->instance};
	XR(get_device(a->instance, &dgi, &g->phys));

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nq, NULL);
	VkQueueFamilyProperties qp[16];
	nq = nq > 16 ? 16 : nq;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nq, qp);
	g->queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < nq && g->queue_family == UINT32_MAX; i++) {
		if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g->queue_family = i;
		}
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = g->queue_family,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkDeviceCreateInfo vdci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
	XrVulkanDeviceCreateInfoKHR xvdci = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
	                                     .systemId = a->system,
	                                     .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	                                     .vulkanPhysicalDevice = g->phys,
	                                     .vulkanCreateInfo = &vdci};
	XR(create_device(a->instance, &xvdci, &g->device, &vr));
	VK(vr);
	vkGetDeviceQueue(g->device, g->queue_family, 0, &g->queue);
	VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                               .queueFamilyIndex = g->queue_family};
	VK(vkCreateCommandPool(g->device, &pci, NULL, &g->pool));
	VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = g->pool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	VK(vkAllocateCommandBuffers(g->device, &cai, &g->cmd));
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK(vkCreateFence(g->device, &fci, NULL, &g->fence));
	create_staging(g, (VkDeviceSize)PANO_W * PANO_H * 4);

	XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	                                       .instance = g->instance,
	                                       .physicalDevice = g->phys,
	                                       .device = g->device,
	                                       .queueFamilyIndex = g->queue_family,
	                                       .queueIndex = 0};
	XrSessionCreateInfo sci = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = a->system};
	XR(xrCreateSession(a->instance, &sci, &a->session));

	XrReferenceSpaceCreateInfo rsci = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	                                   .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	                                   .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
	XR(xrCreateReferenceSpace(a->session, &rsci, &a->local));
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR(xrCreateReferenceSpace(a->session, &rsci, &a->view));
}

static XrSwapchain
create_swapchain(struct app *a, uint32_t w, uint32_t h, VkImage *images, uint32_t max, uint32_t *count)
{
	XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
	                            .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
	                                          XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
	                            .format = a->format,
	                            .sampleCount = 1,
	                            .width = w,
	                            .height = h,
	                            .faceCount = 1,
	                            .arraySize = 1,
	                            .mipCount = 1};
	XrSwapchain sc;
	XR(xrCreateSwapchain(a->session, &ci, &sc));
	XrSwapchainImageVulkan2KHR img[8];
	for (int i = 0; i < 8; i++) {
		img[i] = (XrSwapchainImageVulkan2KHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
	}
	uint32_t n = 0;
	XR(xrEnumerateSwapchainImages(sc, 8, &n, (XrSwapchainImageBaseHeader *)img));
	for (uint32_t i = 0; i < n && i < max; i++) {
		images[i] = img[i].image;
	}
	*count = n;
	return sc;
}

static void
pick_format(struct app *a)
{
	int64_t formats[64];
	uint32_t n = 0;
	XR(xrEnumerateSwapchainFormats(a->session, 64, &n, formats));
	// The cameras deliver gamma-encoded Y8: an sRGB format shows it unchanged.
	const int64_t prefer[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
	                          VK_FORMAT_B8G8R8A8_UNORM};
	for (int p = 0; p < 4; p++) {
		for (uint32_t i = 0; i < n; i++) {
			if (formats[i] == prefer[p]) {
				a->format = prefer[p];
				a->bgra = prefer[p] == VK_FORMAT_B8G8R8A8_SRGB || prefer[p] == VK_FORMAT_B8G8R8A8_UNORM;
				LOG("swapchain format %lld\n", (long long)a->format);
				return;
			}
		}
	}
	LOG("no 8-bit RGBA swapchain format\n");
	exit(1);
}


/*
 * The cube: six quad faces sharing one image strip, only the faces toward the viewer submitted.
 */

static const uint32_t face_rgb[6] = {0xd03030, 0x701818, 0x30c030, 0x186018, 0x3050e0, 0x182870};

static void
upload_faces(struct app *a)
{
	uint32_t *px = a->gpu.staging_ptr;
	for (int f = 0; f < 6; f++) {
		uint32_t c = face_rgb[f];
		uint32_t r = c >> 16 & 0xff, gr = c >> 8 & 0xff, b = c & 0xff;
		uint32_t v = a->bgra ? (0xff000000u | r << 16 | gr << 8 | b) : (0xff000000u | b << 16 | gr << 8 | r);
		for (int y = 0; y < FACE_PX; y++) {
			for (int x = 0; x < FACE_PX; x++) {
				// darker 2 px border so the edges read
				bool edge = x < 2 || y < 2 || x >= FACE_PX - 2 || y >= FACE_PX - 2;
				px[y * FACE_PX * 6 + f * FACE_PX + x] = edge ? 0xff101010u : v;
			}
		}
	}
	uint32_t idx;
	XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XR(xrAcquireSwapchainImage(a->faces, &ai, &idx));
	XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
	XR(xrWaitSwapchainImage(a->faces, &wi));
	upload(&a->gpu, a->face_image, FACE_PX * 6, FACE_PX);
	XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	XR(xrReleaseSwapchainImage(a->faces, &ri));
}

static int
cube_layers(struct app *a, XrVector3f eye, XrCompositionLayerQuad *out)
{
	const XrVector3f center = {0.0f, -0.15f, -1.0f};
	XrQuaternionf cube = quat_mul(quat_axis_angle(0, 1, 0, 0.6f), quat_axis_angle(1, 0, 0, 0.35f));
	// Rotations taking the quad normal (+Z) to +X, -X, +Y, -Y, +Z, -Z.
	const XrQuaternionf face_rot[6] = {
	    quat_axis_angle(0, 1, 0, (float)M_PI / 2), quat_axis_angle(0, 1, 0, -(float)M_PI / 2),
	    quat_axis_angle(1, 0, 0, -(float)M_PI / 2), quat_axis_angle(1, 0, 0, (float)M_PI / 2),
	    quat_axis_angle(0, 1, 0, 0), quat_axis_angle(0, 1, 0, (float)M_PI),
	};
	int n = 0;
	for (int f = 0; f < 6; f++) {
		XrQuaternionf q = quat_mul(cube, face_rot[f]);
		XrVector3f nrm = quat_rotate(q, (XrVector3f){0, 0, 1});
		XrVector3f c = {center.x + nrm.x * CUBE_SIZE / 2, center.y + nrm.y * CUBE_SIZE / 2,
		                center.z + nrm.z * CUBE_SIZE / 2};
		float facing = nrm.x * (eye.x - c.x) + nrm.y * (eye.y - c.y) + nrm.z * (eye.z - c.z);
		if (facing <= 0) {
			continue;
		}
		out[n++] = (XrCompositionLayerQuad){
		    .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
		    .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
		    .space = a->local,
		    .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
		    .subImage = {.swapchain = a->faces,
		                 .imageRect = {.offset = {f * FACE_PX, 0}, .extent = {FACE_PX, FACE_PX}}},
		    .pose = {.orientation = q, .position = c},
		    .size = {CUBE_SIZE, CUBE_SIZE},
		};
	}
	return n;
}


/*
 * Main loop.
 */

int
main(void)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	struct q1cam_shm *shm = open_cameras();
	uint32_t *lut = build_lut(shm);

	struct app a = {0};
	init_xr(&a);
	pick_format(&a);
	a.pano = create_swapchain(&a, PANO_W, PANO_H, a.pano_images, 8, &a.pano_count);
	uint32_t nf;
	a.faces = create_swapchain(&a, FACE_PX * 6, FACE_PX, &a.face_image, 1, &nf);

	bool running = false, faces_done = false, exit_requested = false;
	uint64_t shown_seq = 0, frames = 0, cam_frames = 0;
	int64_t shown_capture_ns = 0;
	for (;;) {
		heartbeat(shm);
		if (quit_requested && running && !exit_requested) {
			xrRequestExitSession(a.session);
			exit_requested = true;
		}
		XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
		bool done = false;
		while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (s == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
					                         .primaryViewConfigurationType =
					                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
					XR(xrBeginSession(a.session, &bi));
					running = true;
					LOG("session running; Ctrl-C to quit\n");
				} else if (s == XR_SESSION_STATE_STOPPING) {
					XR(xrEndSession(a.session));
					running = false;
					done = true;
				} else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
					done = true;
				}
			} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				done = true;
			}
			ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (done || (quit_requested && !running)) {
			break;
		}
		if (!running) {
			usleep(10 * 1000);
			continue;
		}
		if (!faces_done) {
			upload_faces(&a);
			faces_done = true;
		}

		XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
		XrFrameWaitInfo fwi = {.type = XR_TYPE_FRAME_WAIT_INFO};
		XR(xrWaitFrame(a.session, &fwi, &fs));
		XrFrameBeginInfo fbi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
		XR(xrBeginFrame(a.session, &fbi));

		const XrCompositionLayerBaseHeader *layers[8];
		uint32_t nlayers = 0;
		XrCompositionLayerEquirect2KHR pano;
		XrCompositionLayerQuad quads[6];
		if (fs.shouldRender) {
			// Newest camera frameset -> staging buffer.
			uint64_t seq = __atomic_load_n(&shm->latest, __ATOMIC_ACQUIRE);
			if (seq != 0 && seq != shown_seq) {
				const struct q1cam_slot *s = &shm->slot[seq % Q1CAM_SLOTS];
				int64_t ts = s->timestamp_ns;
				remap(lut, s, a.gpu.staging_ptr);
				if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) == seq) {
					shown_seq = seq;
					shown_capture_ns = ts;
					cam_frames++;
				}
			}
			uint32_t idx;
			XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			XR(xrAcquireSwapchainImage(a.pano, &ai, &idx));
			XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
			                               .timeout = XR_INFINITE_DURATION};
			XR(xrWaitSwapchainImage(a.pano, &wi));
			upload(&a.gpu, a.pano_images[idx], PANO_W, PANO_H);
			XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			XR(xrReleaseSwapchainImage(a.pano, &ri));

			// The panorama keeps the head orientation it was captured with.
			XrSpaceLocation now = {.type = XR_TYPE_SPACE_LOCATION};
			XR(xrLocateSpace(a.view, a.local, fs.predictedDisplayTime, &now));
			XrPosef pano_pose = now.pose;
			if (shown_capture_ns != 0) {
				struct timespec tsp = {.tv_sec = shown_capture_ns / 1000000000,
				                       .tv_nsec = shown_capture_ns % 1000000000};
				XrTime capture;
				XrSpaceLocation then = {.type = XR_TYPE_SPACE_LOCATION};
				if (XR_SUCCEEDED(a.convert_time(a.instance, &tsp, &capture)) &&
				    XR_SUCCEEDED(xrLocateSpace(a.view, a.local, capture, &then)) &&
				    (then.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
					pano_pose.orientation = then.pose.orientation;
				}
			}
			pano = (XrCompositionLayerEquirect2KHR){
			    .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
			    .space = a.local,
			    .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
			    .subImage = {.swapchain = a.pano, .imageRect = {.extent = {PANO_W, PANO_H}}},
			    .pose = pano_pose,
			    .radius = INFINITY,
			    .centralHorizontalAngle = 2 * (float)M_PI,
			    .upperVerticalAngle = (float)M_PI / 2,
			    .lowerVerticalAngle = -(float)M_PI / 2,
			};
			layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&pano;
			int nq = cube_layers(&a, now.pose.position, quads);
			for (int i = 0; i < nq; i++) {
				layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quads[i];
			}
		}
		XrFrameEndInfo fei = {.type = XR_TYPE_FRAME_END_INFO,
		                      .displayTime = fs.predictedDisplayTime,
		                      .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		                      .layerCount = nlayers,
		                      .layers = layers};
		XR(xrEndFrame(a.session, &fei));
		if (++frames % 720 == 0) {
			LOG("%llu frames, %llu camera framesets shown\n", (unsigned long long)frames,
			    (unsigned long long)cam_frames);
		}
	}

	xrDestroySession(a.session);
	xrDestroyInstance(a.instance);
	LOG("bye\n");
	return 0;
}
