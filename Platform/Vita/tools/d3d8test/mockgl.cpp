// Stands in for vitaGL when the Direct3D 8 layer is built on a PC (see driver.cpp).
//
// It keeps everything the layer sets, the way a fixed function pipeline would: switches,
// matrices, material, lights, the combiner of each texture unit, textures and buffers with
// what is in them, and the vertex arrays. At each call that draws, clears or shows a frame
// it writes to the trace what has changed since the call before, and for a draw call what
// the vertices and indices it would read hash to. Two builds of the layer that are given
// the same Direct3D calls have to write the same trace, however differently they go about
// telling vitaGL: the order of the calls between two draw calls does not show, what they
// add up to does.
//
// Checked on the way: every call comes from one thread, no draw call reads past the end of
// a buffer or outside the range of vertices it names, and no texture or buffer is used
// after it was deleted.
#include <vitaGL.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>

#include <VitaDiagnostics.h>

#include "mockgl.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{

enum { UNITS = 4, MAX_REPORTED_ERRORS = 20 };

FILE *s_trace = nullptr;
unsigned int s_events = 0;
unsigned int s_draws = 0;
unsigned int s_errors = 0;
unsigned int s_threadErrors = 0;
bool s_threadKnown = false;
std::thread::id s_thread;
std::thread::id s_opener;

// Everything that decides what a draw call looks like, by name, and what of it the trace
// has been told.
std::map<std::string, std::string> s_state;
std::map<std::string, std::string> s_written;

GLenum s_matrixMode = GL_MODELVIEW;
float s_modelView[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
int s_activeUnit = 0;
int s_clientUnit = 0;
unsigned int s_programResets = 0;

struct TextureObject
{
	GLenum format = 0;
	int width = 0;
	int height = 0;
	int levels = 0;
	bool generatedLevels = false;
	uint64_t given = 0;				///< hash over the pixels handed over with the levels
	std::vector<uint8_t> reserved;	///< levels that were only reserved; the layer fills them through vglGetTexDataPointer()
	GLint minFilter = 0;
	GLint magFilter = 0;
	GLint wrapS = 0;
	GLint wrapT = 0;
};

std::map<GLuint, TextureObject> s_textures;
GLuint s_nextTexture = 1;	///< never used twice, so that a deleted texture is known as one
GLuint s_boundTexture[UNITS] = {};
bool s_unitEnabled[UNITS] = {};

struct BufferObject
{
	std::vector<uint8_t> data;
};

std::map<GLuint, BufferObject> s_buffers;
GLuint s_nextBuffer = 1;
GLuint s_arrayBuffer = 0;
GLuint s_elementBuffer = 0;

struct ArrayPointer
{
	bool enabled = false;
	const uint8_t *pointer = nullptr;
	GLsizei stride = 0;
	GLuint buffer = 0;	///< the buffer object that was bound when the pointer was set
	GLint components = 2;
};

std::string s_clearColor = "0 0 0 0";
std::string s_clearDepth = "1";
std::string s_clearStencil = "0";

ArrayPointer s_vertices;
ArrayPointer s_normals;
ArrayPointer s_colors;
ArrayPointer s_texCoords[UNITS];

//-----------------------------------------------------------------------------
std::string Text(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::string Text(const char *format, ...)
{
	char text[1024];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	return text;
}

//-----------------------------------------------------------------------------
void Error(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Error(const char *format, ...)
{
	char text[1024];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);

	if (s_errors++ < MAX_REPORTED_ERRORS)
		fprintf(stderr, "mockgl: before event %u: %s\n", s_events, text);
	if (s_trace != nullptr)
		fprintf(s_trace, "ERROR %s\n", text);
}

//-----------------------------------------------------------------------------
/// At the start of everything vitaGL offers: vitaGL is for one thread.
void Enter(const char *function)
{
	const std::thread::id caller = std::this_thread::get_id();
	if (!s_threadKnown)
	{
		s_thread = caller;
		s_threadKnown = true;
	}
	else if (caller != s_thread)
	{
		if (s_threadErrors++ < MAX_REPORTED_ERRORS)
			fprintf(stderr, "mockgl: %s called from a second thread\n", function);
	}
}

#define ENTER() Enter(__func__)

//-----------------------------------------------------------------------------
uint64_t Hash(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}

//-----------------------------------------------------------------------------
std::string HashText(uint64_t hash)
{
	// In two halves: not every C library's printf knows a 64 bit number.
	return Text("%08x%08x", static_cast<unsigned int>(hash >> 32), static_cast<unsigned int>(hash));
}

//-----------------------------------------------------------------------------
std::string Floats(const float *values, int count)
{
	std::string text;
	for (int i = 0; i < count; ++i)
		text += Text(i == 0 ? "%.6g" : " %.6g", values[i]);
	return text;
}

//-----------------------------------------------------------------------------
int Unit(GLenum texture, const char *function)
{
	const int unit = static_cast<int>(texture) - GL_TEXTURE0;
	if (unit < 0 || unit >= UNITS)
	{
		Error("%s with texture unit %d", function, unit);
		return 0;
	}
	return unit;
}

//-----------------------------------------------------------------------------
TextureObject *BoundTexture(const char *function)
{
	const GLuint name = s_boundTexture[s_activeUnit];
	const std::map<GLuint, TextureObject>::iterator found = s_textures.find(name);
	if (found == s_textures.end())
	{
		Error("%s with texture %u bound, which does not exist", function, name);
		return nullptr;
	}
	return &found->second;
}

//-----------------------------------------------------------------------------
std::string DescribeTexture(GLuint name)
{
	if (name == 0)
		return "none";

	const std::map<GLuint, TextureObject>::const_iterator found = s_textures.find(name);
	if (found == s_textures.end())
		return "DELETED";

	const TextureObject &texture = found->second;
	uint64_t pixels = texture.given;
	if (!texture.reserved.empty())
		pixels = pixels * 1099511628211ull + Hash(texture.reserved.data(), texture.reserved.size());

	return Text("format %04x, %dx%d, %d levels%s, pixels %s, filter %x %x, wrap %x %x",
		texture.format, texture.width, texture.height, texture.levels, texture.generatedLevels ? " and generated ones" : "",
		HashText(pixels).c_str(), texture.minFilter, texture.magFilter, texture.wrapS, texture.wrapT);
}

//-----------------------------------------------------------------------------
/// The top level of a texture is given: what it held before is gone, its filter and
/// wrap modes stay.
void StartImage(TextureObject &texture, GLenum format, int width, int height)
{
	texture.format = format;
	texture.width = width;
	texture.height = height;
	texture.levels = 0;
	texture.generatedLevels = false;
	texture.given = 0;
	texture.reserved.clear();
}

//-----------------------------------------------------------------------------
size_t BytesPerPixel(GLenum format, GLenum type)
{
	switch (type)
	{
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_5_5_5_1:
		case GL_UNSIGNED_SHORT_5_6_5:
			return 2;

		case GL_UNSIGNED_BYTE:
			switch (format)
			{
				case GL_RGBA:
				case GL_BGRA:				return 4;
				case GL_RGB:
				case GL_BGR:				return 3;
				case GL_LUMINANCE_ALPHA:	return 2;
				case GL_LUMINANCE:
				case GL_ALPHA:				return 1;
				default:					break;
			}
			break;

		default:
			break;
	}

	Error("pixels of format %04x and type %04x", format, type);
	return 0;
}

//-----------------------------------------------------------------------------
BufferObject *BoundBuffer(GLenum target, const char *function)
{
	const GLuint name = target == GL_ELEMENT_ARRAY_BUFFER ? s_elementBuffer : s_arrayBuffer;
	const std::map<GLuint, BufferObject>::iterator found = s_buffers.find(name);
	if (found == s_buffers.end())
	{
		Error("%s with buffer %u bound, which does not exist", function, name);
		return nullptr;
	}
	return &found->second;
}

//-----------------------------------------------------------------------------
/// The bytes a draw call reads: in memory, or in a buffer object, where the pointer is an
/// offset.
const uint8_t *Resolve(GLuint buffer, const uint8_t *pointer, size_t size, const char *what)
{
	if (buffer == 0)
	{
		if (pointer == nullptr)
			Error("%s read from a null pointer", what);
		return pointer;
	}

	const std::map<GLuint, BufferObject>::const_iterator found = s_buffers.find(buffer);
	if (found == s_buffers.end())
	{
		Error("%s read from buffer %u, which does not exist", what, buffer);
		return nullptr;
	}

	const size_t offset = reinterpret_cast<uintptr_t>(pointer);
	if (offset + size > found->second.data.size())
	{
		Error("%s read from bytes %u to %u of buffer %u, which has %u", what, static_cast<unsigned int>(offset),
			static_cast<unsigned int>(offset + size), buffer, static_cast<unsigned int>(found->second.data.size()));
		return nullptr;
	}

	return found->second.data.data() + offset;
}

//-----------------------------------------------------------------------------
/// Where one of the other arrays starts, seen from the positions. They are views of the
/// same vertices in everything the layer draws.
std::string DescribeAttribute(const char *name, const ArrayPointer &array)
{
	if (!array.enabled)
		return "";

	std::string text = Text(", %s at %d", name, static_cast<int>(array.pointer - s_vertices.pointer));
	if (array.components != 2)
		text += Text(" of %d components", array.components);
	if (array.stride != s_vertices.stride)
		text += Text(" with stride %d", array.stride);
	if ((array.buffer == 0) != (s_vertices.buffer == 0) || (array.buffer != 0 && array.buffer != s_vertices.buffer))
	{
		Error("the %s array is not in the buffer the positions are in", name);
		text += " ELSEWHERE";
	}
	return text;
}

//-----------------------------------------------------------------------------
std::string DescribeArrays(GLuint first, GLuint last)
{
	if (!s_vertices.enabled)
		Error("a draw call without positions");

	const size_t size = static_cast<size_t>(last - first + 1) * s_vertices.stride;
	const uint8_t *bytes = Resolve(s_vertices.buffer, s_vertices.pointer + static_cast<size_t>(first) * s_vertices.stride, size, "vertices");

	std::string text = Text("vertices %u to %u of %d bytes in %s %s", first, last, s_vertices.stride,
		s_vertices.buffer != 0 ? "a buffer" : "memory", HashText(bytes != nullptr ? Hash(bytes, size) : 0).c_str());
	text += DescribeAttribute("normals", s_normals);
	text += DescribeAttribute("colors", s_colors);
	for (int unit = 0; unit < UNITS; ++unit)
		text += DescribeAttribute(Text("coordinates %d", unit).c_str(), s_texCoords[unit]);
	return text;
}

//-----------------------------------------------------------------------------
/// Writes what has changed since the last event, and the event.
void Report(const std::string &event, bool draws)
{
	for (int unit = 0; unit < UNITS; ++unit)
	{
		// What is bound to a unit that is switched off does not show in the picture.
		const std::string texture = s_unitEnabled[unit] ? DescribeTexture(s_boundTexture[unit]) : "off";
		if (draws && texture == "DELETED")
			Error("a draw call with deleted texture %u on unit %d", s_boundTexture[unit], unit);
		s_state[Text("unit %d texture", unit)] = texture;
	}

	// The layer selects "no program" to make vitaGL hand all its values to the shader again.
	s_state["program resets"] = Text("%u", s_programResets);
	s_programResets = 0;

	for (const std::pair<const std::string, std::string> &entry : s_state)
	{
		std::string &written = s_written[entry.first];
		if (written != entry.second)
		{
			fprintf(s_trace, "  %s = %s\n", entry.first.c_str(), entry.second.c_str());
			written = entry.second;
		}
	}

	fprintf(s_trace, "%u %s\n", s_events, event.c_str());
	++s_events;
	if (draws)
		++s_draws;
}

//-----------------------------------------------------------------------------
void SetCapability(GLenum capability, bool enabled)
{
	if (capability == GL_TEXTURE_2D)
		s_unitEnabled[s_activeUnit] = enabled;
	else
		s_state[Text("switch %04x", capability)] = enabled ? "on" : "off";
}

//-----------------------------------------------------------------------------
void SetClientState(GLenum array, bool enabled)
{
	switch (array)
	{
		case GL_VERTEX_ARRAY:
			s_vertices.enabled = enabled;
			break;

		case GL_NORMAL_ARRAY:
			s_normals.enabled = enabled;
			break;

		case GL_COLOR_ARRAY:
			s_colors.enabled = enabled;
			break;

		case GL_TEXTURE_COORD_ARRAY:
			s_texCoords[s_clientUnit].enabled = enabled;
			break;

		default:
			Error("client state %04x", array);
			break;
	}
}

//-----------------------------------------------------------------------------
void LoadMatrix(const float *matrix)
{
	switch (s_matrixMode)
	{
		case GL_PROJECTION:
			s_state["matrix projection"] = Floats(matrix, 16);
			break;

		case GL_MODELVIEW:
			memcpy(s_modelView, matrix, sizeof(s_modelView));
			s_state["matrix model view"] = Floats(matrix, 16);
			break;

		case GL_TEXTURE:
			s_state[Text("unit %d matrix", s_activeUnit)] = Floats(matrix, 16);
			break;

		default:
			Error("a matrix loaded in mode %04x", s_matrixMode);
			break;
	}
}

} // namespace

//-----------------------------------------------------------------------------
bool MockGL_Open(const char *path)
{
	s_opener = std::this_thread::get_id();
	s_trace = fopen(path, "w");
	return s_trace != nullptr;
}

//-----------------------------------------------------------------------------
unsigned int MockGL_Close(bool expectOtherThread)
{
	unsigned int wrong = s_errors + s_threadErrors;

	if (s_threadKnown && (s_thread != s_opener) != expectOtherThread)
	{
		fprintf(stderr, "mockgl: the calls came from %s\n", expectOtherThread ? "the thread of the test, not from a render thread" : "another thread than the test's");
		++wrong;
	}

	fprintf(stderr, "mockgl: %u events, %u of them draw calls; %u errors, %u calls from a second thread\n",
		s_events, s_draws, s_errors, s_threadErrors);

	if (s_trace != nullptr)
	{
		fclose(s_trace);
		s_trace = nullptr;
	}
	return wrong;
}

///////////////////////////////////////////////////////////////////////////////
// vitaGL
///////////////////////////////////////////////////////////////////////////////

GLboolean vglInitExtended(int, int, int, int, SceGxmMultisampleMode)
{
	ENTER();
	return GL_TRUE;
}

void vglWaitVblankStart(GLboolean)
{
	ENTER();
}

#ifdef VGL_TEXCOORD_GEN_EYE_NORMAL
void vglSetFfpShaderCachePath(const char *path)
{
	ENTER();
	s_state["fixed function shader cache"] = path;
}

void vglTexCoordGen(GLenum mode)
{
	ENTER();
	s_state[Text("unit %d coordinates generated", s_activeUnit)] = Text("%x", mode);
}
#endif

size_t vglMemFree(vglMemType)
{
	ENTER();
	return 64 * 1024 * 1024;
}

size_t vglMemTotal(vglMemType)
{
	ENTER();
	return 96 * 1024 * 1024;
}

void vglSwapBuffers(GLboolean)
{
	ENTER();
	Report("show", false);
}

void *vglGetTexDataPointer(GLenum)
{
	ENTER();
	TextureObject *texture = BoundTexture(__func__);
	return texture != nullptr && !texture->reserved.empty() ? texture->reserved.data() : nullptr;
}

// Switches ---------------------------------------------------------------------

void glEnable(GLenum cap)
{
	ENTER();
	SetCapability(cap, true);
}

void glDisable(GLenum cap)
{
	ENTER();
	SetCapability(cap, false);
}

void glEnableClientState(GLenum array)
{
	ENTER();
	SetClientState(array, true);
}

void glDisableClientState(GLenum array)
{
	ENTER();
	SetClientState(array, false);
}

void glActiveTexture(GLenum texture)
{
	ENTER();
	s_activeUnit = Unit(texture, __func__);
}

void glClientActiveTexture(GLenum texture)
{
	ENTER();
	s_clientUnit = Unit(texture, __func__);
}

void glUseProgram(GLuint program)
{
	ENTER();
	if (program == 0)
		++s_programResets;
	else
		Error("program %u selected", program);
}

void glPixelStorei(GLenum pname, GLint param)
{
	ENTER();
	s_state[Text("pixel store %04x", pname)] = Text("%d", param);
}

// Matrices ---------------------------------------------------------------------

void glMatrixMode(GLenum mode)
{
	ENTER();
	s_matrixMode = mode;
}

void glLoadMatrixf(const GLfloat *m)
{
	ENTER();
	LoadMatrix(m);
}

void glLoadIdentity(void)
{
	ENTER();
	const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	LoadMatrix(identity);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	ENTER();
	s_state["viewport"] = Text("%d %d %d %d", x, y, width, height);
}

void glDepthRangef(GLfloat nearVal, GLfloat farVal)
{
	ENTER();
	s_state["depth range"] = Text("%.6g %.6g", nearVal, farVal);
}

// Lighting ---------------------------------------------------------------------

void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
	ENTER();
	const int index = static_cast<int>(light) - GL_LIGHT0;
	const std::string key = Text("light %d %04x", index, pname);

	if (pname == GL_POSITION)
	{
		// Kept in eye space: the model view matrix of the moment is part of what is set.
		float eye[4];
		for (int i = 0; i < 4; ++i)
			eye[i] = s_modelView[i] * params[0] + s_modelView[4 + i] * params[1] + s_modelView[8 + i] * params[2] + s_modelView[12 + i] * params[3];
		s_state[key] = Floats(eye, 4);
	}
	else if (pname == GL_CONSTANT_ATTENUATION || pname == GL_LINEAR_ATTENUATION || pname == GL_QUADRATIC_ATTENUATION)
	{
		s_state[key] = Floats(params, 1);
	}
#ifdef GL_LIGHT_RANGE_VGL
	else if (pname == GL_LIGHT_RANGE_VGL)
	{
		s_state[key] = Floats(params, 1);
	}
#endif
	else
	{
		s_state[key] = Floats(params, 4);
	}
}

void glLightModelfv(GLenum pname, const GLfloat *params)
{
	ENTER();
	s_state[Text("light model %04x", pname)] = Floats(params, 4);
}

void glMaterialfv(GLenum, GLenum pname, const GLfloat *params)
{
	ENTER();
	s_state[Text("material %04x", pname)] = Floats(params, pname == GL_SHININESS ? 1 : 4);
}

void glMaterialf(GLenum, GLenum pname, GLfloat param)
{
	ENTER();
	s_state[Text("material %04x", pname)] = Floats(&param, 1);
}

void glColorMaterial(GLenum, GLenum mode)
{
	ENTER();
	s_state["color material"] = Text("%04x", mode);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	ENTER();
	s_state["color"] = Text("%.6g %.6g %.6g %.6g", red, green, blue, alpha);
}

void glShadeModel(GLenum mode)
{
	ENTER();
	s_state["shade model"] = Text("%04x", mode);
}

void glClipPlane(GLenum plane, const GLdouble *equation)
{
	ENTER();
	s_state[Text("clip plane %d", static_cast<int>(plane) - GL_CLIP_PLANE0)] = Text("%.6g %.6g %.6g %.6g under ",
		equation[0], equation[1], equation[2], equation[3]) + Floats(s_modelView, 16);
}

void glFogf(GLenum pname, GLfloat param)
{
	ENTER();
	s_state[Text("fog %04x", pname)] = Floats(&param, 1);
}

void glFogfv(GLenum pname, const GLfloat *params)
{
	ENTER();
	s_state[Text("fog %04x", pname)] = Floats(params, 4);
}

void glFogi(GLenum pname, const GLint param)
{
	ENTER();
	s_state[Text("fog %04x", pname)] = Text("%x", param);
}

// Pixels -----------------------------------------------------------------------

void glAlphaFunc(GLenum func, GLfloat ref)
{
	ENTER();
	s_state["alpha test"] = Text("%04x %.6g", func, ref);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	ENTER();
	s_state["blend factors"] = Text("%04x %04x", sfactor, dfactor);
}

void glBlendEquation(GLenum mode)
{
	ENTER();
	s_state["blend equation"] = Text("%04x", mode);
}

void glDepthFunc(GLenum func)
{
	ENTER();
	s_state["depth test"] = Text("%04x", func);
}

void glDepthMask(GLboolean flag)
{
	ENTER();
	s_state["depth mask"] = Text("%d", flag);
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
	ENTER();
	s_state["polygon offset"] = Text("%.6g %.6g", factor, units);
}

void glPolygonMode(GLenum, GLenum mode)
{
	ENTER();
	s_state["polygon mode"] = Text("%04x", mode);
}

void glFrontFace(GLenum mode)
{
	ENTER();
	s_state["front face"] = Text("%04x", mode);
}

void glCullFace(GLenum mode)
{
	ENTER();
	s_state["cull face"] = Text("%04x", mode);
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
	ENTER();
	s_state["color mask"] = Text("%d %d %d %d", red, green, blue, alpha);
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
	ENTER();
	s_state["stencil test"] = Text("%04x %d %08x", func, ref, mask);
}

void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
{
	ENTER();
	s_state["stencil operations"] = Text("%04x %04x %04x", sfail, dpfail, dppass);
}

void glStencilMask(GLuint mask)
{
	ENTER();
	s_state["stencil mask"] = Text("%08x", mask);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	ENTER();
	s_state["scissor"] = Text("%d %d %d %d", x, y, width, height);
}

void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	ENTER();
	s_clearColor = Text("%.6g %.6g %.6g %.6g", red, green, blue, alpha);
}

void glClearDepthf(GLclampf depth)
{
	ENTER();
	s_clearDepth = Text("%.6g", depth);
}

void glClearStencil(GLint s)
{
	ENTER();
	s_clearStencil = Text("%d", s);
}

void glClear(GLbitfield mask)
{
	ENTER();

	// What the buffers are cleared to only shows when they are.
	Report(Text("clear %x to color %s, depth %s, stencil %s", mask, s_clearColor.c_str(), s_clearDepth.c_str(), s_clearStencil.c_str()), false);
}

// Textures ---------------------------------------------------------------------

void glGenTextures(GLsizei n, GLuint *textures)
{
	ENTER();
	for (GLsizei i = 0; i < n; ++i)
	{
		textures[i] = s_nextTexture++;
		s_textures[textures[i]] = TextureObject();
	}
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
	ENTER();
	for (GLsizei i = 0; i < n; ++i)
	{
		if (s_textures.erase(textures[i]) == 0)
			Error("texture %u deleted, which does not exist", textures[i]);
	}
}

void glBindTexture(GLenum, GLuint texture)
{
	ENTER();
	if (texture != 0 && s_textures.find(texture) == s_textures.end())
		Error("texture %u bound, which does not exist", texture);
	s_boundTexture[s_activeUnit] = texture;
}

void glTexParameteri(GLenum, GLenum pname, GLint param)
{
	ENTER();
	TextureObject *texture = BoundTexture(__func__);
	if (texture == nullptr)
		return;

	switch (pname)
	{
		case GL_TEXTURE_MIN_FILTER:	texture->minFilter = param; break;
		case GL_TEXTURE_MAG_FILTER:	texture->magFilter = param; break;
		case GL_TEXTURE_WRAP_S:		texture->wrapS = param; break;
		case GL_TEXTURE_WRAP_T:		texture->wrapT = param; break;
		default:					Error("texture parameter %04x", pname); break;
	}
}

void glTexImage2D(GLenum, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint, GLenum format, GLenum type, const GLvoid *data)
{
	ENTER();
	TextureObject *texture = BoundTexture(__func__);
	if (texture == nullptr)
		return;

	if (level == 0)
		StartImage(*texture, static_cast<GLenum>(internalFormat), width, height);

	++texture->levels;
	if (data == nullptr)
		Error("pixels of an uncompressed texture without data");
	else
		texture->given = texture->given * 1099511628211ull + Hash(data, static_cast<size_t>(width) * height * BytesPerPixel(format, type));
}

void glCompressedTexImage2D(GLenum, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint, GLsizei imageSize, const void *data)
{
	ENTER();
	TextureObject *texture = BoundTexture(__func__);
	if (texture == nullptr)
		return;

	if (level == 0)
		StartImage(*texture, internalformat, width, height);

	++texture->levels;
	if (data != nullptr)
		texture->given = texture->given * 1099511628211ull + Hash(data, static_cast<size_t>(imageSize));
	else
		// As in vitaGL, the texture's memory moves when a level is added.
		texture->reserved.resize(texture->reserved.size() + static_cast<size_t>(imageSize));
}

void glGenerateMipmap(GLenum)
{
	ENTER();
	TextureObject *texture = BoundTexture(__func__);
	if (texture != nullptr)
		texture->generatedLevels = true;
}

void glTexEnvi(GLenum, GLenum pname, GLint param)
{
	ENTER();
	s_state[Text("unit %d combiner %04x", s_activeUnit, pname)] = Text("%x", param);
}

void glTexEnvfv(GLenum, GLenum pname, GLfloat *param)
{
	ENTER();
	s_state[Text("unit %d combiner %04x", s_activeUnit, pname)] = Floats(param, 4);
}

// Buffers ----------------------------------------------------------------------

void glGenBuffers(GLsizei n, GLuint *buffers)
{
	ENTER();
	for (GLsizei i = 0; i < n; ++i)
	{
		buffers[i] = s_nextBuffer++;
		s_buffers[buffers[i]] = BufferObject();
	}
}

void glDeleteBuffers(GLsizei n, const GLuint *gl_buffers)
{
	ENTER();
	for (GLsizei i = 0; i < n; ++i)
	{
		if (s_buffers.erase(gl_buffers[i]) == 0)
			Error("buffer %u deleted, which does not exist", gl_buffers[i]);
	}
}

void glBindBuffer(GLenum target, GLuint buffer)
{
	ENTER();
	if (buffer != 0 && s_buffers.find(buffer) == s_buffers.end())
		Error("buffer %u bound, which does not exist", buffer);

	if (target == GL_ELEMENT_ARRAY_BUFFER)
		s_elementBuffer = buffer;
	else if (target == GL_ARRAY_BUFFER)
		s_arrayBuffer = buffer;
	else
		Error("a buffer bound to %04x", target);
}

void glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum)
{
	ENTER();
	BufferObject *buffer = BoundBuffer(target, __func__);
	if (buffer == nullptr)
		return;

	buffer->data.assign(static_cast<size_t>(size), 0);
	if (data != nullptr)
		memcpy(buffer->data.data(), data, static_cast<size_t>(size));
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	ENTER();
	BufferObject *buffer = BoundBuffer(target, __func__);
	if (buffer == nullptr)
		return;

	if (static_cast<size_t>(offset) + static_cast<size_t>(size) > buffer->data.size())
	{
		Error("bytes %u to %u of a buffer of %u replaced", static_cast<unsigned int>(offset),
			static_cast<unsigned int>(offset + size), static_cast<unsigned int>(buffer->data.size()));
		return;
	}

	memcpy(buffer->data.data() + offset, data, static_cast<size_t>(size));
}

// Vertex arrays ----------------------------------------------------------------

void glVertexPointer(GLint, GLenum, GLsizei stride, const GLvoid *pointer)
{
	ENTER();
	s_vertices.pointer = static_cast<const uint8_t *>(pointer);
	s_vertices.stride = stride;
	s_vertices.buffer = s_arrayBuffer;
}

void glNormalPointer(GLenum, GLsizei stride, const void *pointer)
{
	ENTER();
	s_normals.pointer = static_cast<const uint8_t *>(pointer);
	s_normals.stride = stride;
	s_normals.buffer = s_arrayBuffer;
}

void glColorPointer(GLint, GLenum, GLsizei stride, const GLvoid *pointer)
{
	ENTER();
	s_colors.pointer = static_cast<const uint8_t *>(pointer);
	s_colors.stride = stride;
	s_colors.buffer = s_arrayBuffer;

	// vitaGL remembers the buffer of the last color pointer and reads the ambient material
	// from it while the array is switched off; the layer works around that.
	s_state["buffer of the last color pointer"] = s_arrayBuffer != 0 ? "one" : "none";
}

void glTexCoordPointer(GLint size, GLenum, GLsizei stride, const GLvoid *pointer)
{
	ENTER();
	s_texCoords[s_clientUnit].pointer = static_cast<const uint8_t *>(pointer);
	s_texCoords[s_clientUnit].stride = stride;
	s_texCoords[s_clientUnit].buffer = s_arrayBuffer;
	s_texCoords[s_clientUnit].components = size;
}

/// With D3D8TEST_SLOW_DRAWS=<microseconds> in the environment every draw call takes that
/// long. The game's thread is then a frame ahead of the one that draws all the time, and
/// whatever it writes over too early is caught as vertices that hash to something else.
void TakeTimeToDraw()
{
	static const char *const wanted = getenv("D3D8TEST_SLOW_DRAWS");
	static const int microseconds = wanted != nullptr ? atoi(wanted) : 0;
	if (microseconds <= 0)
		return;

	const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
	while (std::chrono::steady_clock::now() < until)
	{
	}
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	ENTER();
	TakeTimeToDraw();
	Report(Text("draw %d vertices as %x: ", count, mode)
		+ DescribeArrays(static_cast<GLuint>(first), static_cast<GLuint>(first + count - 1)), true);
}

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
{
	ENTER();
	TakeTimeToDraw();

	const size_t indexSize = type == GL_UNSIGNED_INT ? 4 : 2;
	const size_t size = static_cast<size_t>(count) * indexSize;
	const uint8_t *bytes = Resolve(s_elementBuffer, static_cast<const uint8_t *>(indices), size, "indices");

	if (bytes != nullptr)
	{
		for (GLsizei i = 0; i < count; ++i)
		{
			uint32_t index = 0;
			memcpy(&index, bytes + i * indexSize, indexSize);
			if (index < start || index > end)
			{
				Error("index %u in a draw call that names vertices %u to %u", index, start, end);
				break;
			}
		}
	}

	Report(Text("draw %d indices as %x, %s %s: ", count, mode, s_elementBuffer != 0 ? "in a buffer" : "in memory",
		HashText(bytes != nullptr ? Hash(bytes, size) : 0).c_str()) + DescribeArrays(start, end), true);
}

///////////////////////////////////////////////////////////////////////////////
// The rest of the console
///////////////////////////////////////////////////////////////////////////////

int sceGxmTransferFinish(void)
{
	return 0;
}

SceUInt64 sceKernelGetProcessTimeWide(void)
{
	return VitaDiagnostics::now();
}

namespace VitaDiagnostics
{

bool g_profiling = false;
uint32_t g_drawCalls = 0;

uint64_t now()
{
	// Never 0, which the timers take for "not measuring".
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count()) + 1;
}

MemoryState getMemoryState()
{
	return MemoryState();
}

uint64_t takeTime(TimeSlot)
{
	return 0;
}

void logNamedTimers(uint32_t)
{
}

} // namespace VitaDiagnostics
