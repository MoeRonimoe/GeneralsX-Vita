/*
**	Direct3D 8 on vitaGL
**	Part of the PS Vita port of Command & Conquer Generals.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// VitaD3D8GL.h ///////////////////////////////////////////////////////////////
// Sends this layer's calls to vitaGL through the render thread.
//
// A source file that includes this header after everything else keeps writing
// glEnable(...) and so on, and each of those calls becomes an entry in the
// stream of VitaD3D8RenderThread.h, or the call itself when no render thread
// runs. There are three kinds:
//
//  - Calls with plain values, and calls with a pointer that is really an offset
//    into a buffer object, are written as they are.
//  - Calls with a few numbers behind a pointer (a matrix, a color) take a copy
//    of the numbers.
//  - Calls that hand over a block of data or return something are made by the
//    render thread while the game's thread waits, unless the data is small
//    enough to be copied into the stream (buffer uploads).
//
// The caller has one duty: a pointer to vertices or indices in system memory
// has to come from RenderThread::Keep(), because the draw call that reads them
// is made later. Device::Draw*() do that.
//
// EVERY vitaGL function the layer calls has to be listed here. One that is not
// would be called by the game's thread while the render thread is inside
// vitaGL, which vitaGL does not survive. The five that are left out on purpose
// (vglInitExtended, vglSetFfpShaderCachePath, vglWaitVblankStart,
// vglGetTexDataPointer, sceGxmTransferFinish) are only called from inside
// RenderThread::Sync().
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "VitaD3D8.h"
#include "VitaD3D8RenderThread.h"

namespace VitaD3D8
{
namespace QueuedGL
{

template <typename Value, size_t Count>
struct Values
{
	Value v[Count];
};

template <typename Value, size_t Count>
inline Values<Value, Count> CopyValues(const Value *source, size_t used = Count)
{
	Values<Value, Count> values = {};
	memcpy(values.v, source, used * sizeof(Value));
	return values;
}

//-----------------------------------------------------------------------------
inline void LoadMatrixf(const GLfloat *matrix)
{
	const Values<GLfloat, 16> values = CopyValues<GLfloat, 16>(matrix);
	RenderThread::Defer([values] { ::glLoadMatrixf(values.v); });
}

//-----------------------------------------------------------------------------
inline void Lightfv(GLenum light, GLenum name, const GLfloat *parameters)
{
	// The attenuation factors are single numbers; a color or a position is four. vitaGL
	// has no spot lights.
	const bool single = name == GL_CONSTANT_ATTENUATION || name == GL_LINEAR_ATTENUATION || name == GL_QUADRATIC_ATTENUATION;
	const Values<GLfloat, 4> values = CopyValues<GLfloat, 4>(parameters, single ? 1 : 4);
	RenderThread::Defer([light, name, values] { ::glLightfv(light, name, values.v); });
}

//-----------------------------------------------------------------------------
inline void Materialfv(GLenum face, GLenum name, const GLfloat *parameters)
{
	const Values<GLfloat, 4> values = CopyValues<GLfloat, 4>(parameters, name == GL_SHININESS ? 1 : 4);
	RenderThread::Defer([face, name, values] { ::glMaterialfv(face, name, values.v); });
}

//-----------------------------------------------------------------------------
inline void LightModelfv(GLenum name, const GLfloat *parameters)
{
	const Values<GLfloat, 4> values = CopyValues<GLfloat, 4>(parameters);
	RenderThread::Defer([name, values] { ::glLightModelfv(name, values.v); });
}

//-----------------------------------------------------------------------------
inline void Fogfv(GLenum name, const GLfloat *parameters)
{
	const Values<GLfloat, 4> values = CopyValues<GLfloat, 4>(parameters);
	RenderThread::Defer([name, values] { ::glFogfv(name, values.v); });
}

//-----------------------------------------------------------------------------
inline void TexEnvfv(GLenum target, GLenum name, const GLfloat *parameters)
{
	const Values<GLfloat, 4> values = CopyValues<GLfloat, 4>(parameters);
	RenderThread::Defer([target, name, values] { ::glTexEnvfv(target, name, const_cast<GLfloat *>(values.v)); });
}

//-----------------------------------------------------------------------------
inline void ClipPlane(GLenum plane, const GLdouble *equation)
{
	const Values<GLdouble, 4> values = CopyValues<GLdouble, 4>(equation);
	RenderThread::Defer([plane, values] { ::glClipPlane(plane, values.v); });
}

//-----------------------------------------------------------------------------
// The names are needed at once.
inline void GenTextures(GLsizei count, GLuint *textures)
{
	RenderThread::Sync([count, textures] { ::glGenTextures(count, textures); });
}

inline void GenBuffers(GLsizei count, GLuint *buffers)
{
	RenderThread::Sync([count, buffers] { ::glGenBuffers(count, buffers); });
}

//-----------------------------------------------------------------------------
inline void DeleteTexture(GLuint texture)
{
	::glDeleteTextures(1, &texture);
}

inline void DeleteTextures(GLsizei count, const GLuint *textures)
{
	for (GLsizei i = 0; i < count; ++i)
		RenderThread::Call(DeleteTexture, textures[i]);
}

inline void DeleteBuffer(GLuint buffer)
{
	::glDeleteBuffers(1, &buffer);
}

inline void DeleteBuffers(GLsizei count, const GLuint *buffers)
{
	for (GLsizei i = 0; i < count; ++i)
		RenderThread::Call(DeleteBuffer, buffers[i]);
}

//-----------------------------------------------------------------------------
// Pixels are megabytes, and whoever uploads a texture goes on to ask vitaGL where it
// put them (Texture::UploadCompressed), so texture uploads are waited for.
inline void TexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border,
	GLenum format, GLenum type, const GLvoid *data)
{
	RenderThread::Sync([=] { ::glTexImage2D(target, level, internalFormat, width, height, border, format, type, data); });
}

inline void CompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border,
	GLsizei imageSize, const void *data)
{
	RenderThread::Sync([=] { ::glCompressedTexImage2D(target, level, internalFormat, width, height, border, imageSize, data); });
}

//-----------------------------------------------------------------------------
// The engine changes parts of its buffers while it draws (the terrain, when the view
// moves), so these are not waited for unless they are large.
inline void BufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage)
{
	if (RenderThread::g_queueing && data != nullptr && static_cast<size_t>(size) > RenderThread::MAX_KEPT_BYTES)
	{
		RenderThread::Sync([=] { ::glBufferData(target, size, data, usage); });
		return;
	}

	RenderThread::Call(::glBufferData, target, size, RenderThread::Keep(data, static_cast<size_t>(size)), usage);
}

inline void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	if (RenderThread::g_queueing && data != nullptr && static_cast<size_t>(size) > RenderThread::MAX_KEPT_BYTES)
	{
		RenderThread::Sync([=] { ::glBufferSubData(target, offset, size, data); });
		return;
	}

	RenderThread::Call(::glBufferSubData, target, offset, size, RenderThread::Keep(data, static_cast<size_t>(size)));
}

//-----------------------------------------------------------------------------
inline void SwapBuffers(GLboolean hasCommonDialog)
{
	RenderThread::Call(::vglSwapBuffers, hasCommonDialog);
	RenderThread::EndFrame();
}

//-----------------------------------------------------------------------------
// vitaGL walks its heaps for these numbers, which it must not do while it allocates.
inline size_t MemFree(vglMemType type)
{
	size_t bytes = 0;
	RenderThread::Sync([&bytes, type] { bytes = ::vglMemFree(type); });
	return bytes;
}

inline size_t MemTotal(vglMemType type)
{
	size_t bytes = 0;
	RenderThread::Sync([&bytes, type] { bytes = ::vglMemTotal(type); });
	return bytes;
}

} // namespace QueuedGL
} // namespace VitaD3D8

// Calls with plain values. A macro is not replaced inside its own replacement, so the name
// after "::" is the function.
#define glActiveTexture(...)		::VitaD3D8::RenderThread::Call(::glActiveTexture, __VA_ARGS__)
#define glAlphaFunc(...)			::VitaD3D8::RenderThread::Call(::glAlphaFunc, __VA_ARGS__)
#define glBindBuffer(...)			::VitaD3D8::RenderThread::Call(::glBindBuffer, __VA_ARGS__)
#define glBindTexture(...)			::VitaD3D8::RenderThread::Call(::glBindTexture, __VA_ARGS__)
#define glBlendEquation(...)		::VitaD3D8::RenderThread::Call(::glBlendEquation, __VA_ARGS__)
#define glBlendFunc(...)			::VitaD3D8::RenderThread::Call(::glBlendFunc, __VA_ARGS__)
#define glClear(...)				::VitaD3D8::RenderThread::Call(::glClear, __VA_ARGS__)
#define glClearColor(...)			::VitaD3D8::RenderThread::Call(::glClearColor, __VA_ARGS__)
#define glClearDepthf(...)			::VitaD3D8::RenderThread::Call(::glClearDepthf, __VA_ARGS__)
#define glClearStencil(...)			::VitaD3D8::RenderThread::Call(::glClearStencil, __VA_ARGS__)
#define glClientActiveTexture(...)	::VitaD3D8::RenderThread::Call(::glClientActiveTexture, __VA_ARGS__)
#define glColor4f(...)				::VitaD3D8::RenderThread::Call(::glColor4f, __VA_ARGS__)
#define glColorMask(...)			::VitaD3D8::RenderThread::Call(::glColorMask, __VA_ARGS__)
#define glColorMaterial(...)		::VitaD3D8::RenderThread::Call(::glColorMaterial, __VA_ARGS__)
#define glCullFace(...)				::VitaD3D8::RenderThread::Call(::glCullFace, __VA_ARGS__)
#define glDepthFunc(...)			::VitaD3D8::RenderThread::Call(::glDepthFunc, __VA_ARGS__)
#define glDepthMask(...)			::VitaD3D8::RenderThread::Call(::glDepthMask, __VA_ARGS__)
#define glDepthRangef(...)			::VitaD3D8::RenderThread::Call(::glDepthRangef, __VA_ARGS__)
#define glDisable(...)				::VitaD3D8::RenderThread::Call(::glDisable, __VA_ARGS__)
#define glDisableClientState(...)	::VitaD3D8::RenderThread::Call(::glDisableClientState, __VA_ARGS__)
#define glDrawArrays(...)			::VitaD3D8::RenderThread::Call(::glDrawArrays, __VA_ARGS__)
#define glEnable(...)				::VitaD3D8::RenderThread::Call(::glEnable, __VA_ARGS__)
#define glEnableClientState(...)	::VitaD3D8::RenderThread::Call(::glEnableClientState, __VA_ARGS__)
#define glFogf(...)					::VitaD3D8::RenderThread::Call(::glFogf, __VA_ARGS__)
#define glFogi(...)					::VitaD3D8::RenderThread::Call(::glFogi, __VA_ARGS__)
#define glFrontFace(...)			::VitaD3D8::RenderThread::Call(::glFrontFace, __VA_ARGS__)
#define glGenerateMipmap(...)		::VitaD3D8::RenderThread::Call(::glGenerateMipmap, __VA_ARGS__)
#define glLoadIdentity()			::VitaD3D8::RenderThread::Call(::glLoadIdentity)
#define glMaterialf(...)			::VitaD3D8::RenderThread::Call(::glMaterialf, __VA_ARGS__)
#define glMatrixMode(...)			::VitaD3D8::RenderThread::Call(::glMatrixMode, __VA_ARGS__)
#define glPixelStorei(...)			::VitaD3D8::RenderThread::Call(::glPixelStorei, __VA_ARGS__)
#define glPolygonMode(...)			::VitaD3D8::RenderThread::Call(::glPolygonMode, __VA_ARGS__)
#define glPolygonOffset(...)		::VitaD3D8::RenderThread::Call(::glPolygonOffset, __VA_ARGS__)
#define glScissor(...)				::VitaD3D8::RenderThread::Call(::glScissor, __VA_ARGS__)
#define glShadeModel(...)			::VitaD3D8::RenderThread::Call(::glShadeModel, __VA_ARGS__)
#define glStencilFunc(...)			::VitaD3D8::RenderThread::Call(::glStencilFunc, __VA_ARGS__)
#define glStencilMask(...)			::VitaD3D8::RenderThread::Call(::glStencilMask, __VA_ARGS__)
#define glStencilOp(...)			::VitaD3D8::RenderThread::Call(::glStencilOp, __VA_ARGS__)
#define glTexEnvi(...)				::VitaD3D8::RenderThread::Call(::glTexEnvi, __VA_ARGS__)
#define glTexParameteri(...)		::VitaD3D8::RenderThread::Call(::glTexParameteri, __VA_ARGS__)
#define glUseProgram(...)			::VitaD3D8::RenderThread::Call(::glUseProgram, __VA_ARGS__)
#define glViewport(...)				::VitaD3D8::RenderThread::Call(::glViewport, __VA_ARGS__)

// Calls with a pointer that is an offset into the buffer object that is bound, or that
// came from RenderThread::Keep().
#define glColorPointer(...)			::VitaD3D8::RenderThread::Call(::glColorPointer, __VA_ARGS__)
#define glDrawRangeElements(...)	::VitaD3D8::RenderThread::Call(::glDrawRangeElements, __VA_ARGS__)
#define glNormalPointer(...)		::VitaD3D8::RenderThread::Call(::glNormalPointer, __VA_ARGS__)
#define glTexCoordPointer(...)		::VitaD3D8::RenderThread::Call(::glTexCoordPointer, __VA_ARGS__)
#define glVertexPointer(...)		::VitaD3D8::RenderThread::Call(::glVertexPointer, __VA_ARGS__)

// Calls that need more than that, see above.
#define glBufferData(...)			::VitaD3D8::QueuedGL::BufferData(__VA_ARGS__)
#define glBufferSubData(...)		::VitaD3D8::QueuedGL::BufferSubData(__VA_ARGS__)
#define glClipPlane(...)			::VitaD3D8::QueuedGL::ClipPlane(__VA_ARGS__)
#define glCompressedTexImage2D(...)	::VitaD3D8::QueuedGL::CompressedTexImage2D(__VA_ARGS__)
#define glDeleteBuffers(...)		::VitaD3D8::QueuedGL::DeleteBuffers(__VA_ARGS__)
#define glDeleteTextures(...)		::VitaD3D8::QueuedGL::DeleteTextures(__VA_ARGS__)
#define glFogfv(...)				::VitaD3D8::QueuedGL::Fogfv(__VA_ARGS__)
#define glGenBuffers(...)			::VitaD3D8::QueuedGL::GenBuffers(__VA_ARGS__)
#define glGenTextures(...)			::VitaD3D8::QueuedGL::GenTextures(__VA_ARGS__)
#define glLightfv(...)				::VitaD3D8::QueuedGL::Lightfv(__VA_ARGS__)
#define glLightModelfv(...)			::VitaD3D8::QueuedGL::LightModelfv(__VA_ARGS__)
#define glLoadMatrixf(...)			::VitaD3D8::QueuedGL::LoadMatrixf(__VA_ARGS__)
#define glMaterialfv(...)			::VitaD3D8::QueuedGL::Materialfv(__VA_ARGS__)
#define glTexEnvfv(...)				::VitaD3D8::QueuedGL::TexEnvfv(__VA_ARGS__)
#define glTexImage2D(...)			::VitaD3D8::QueuedGL::TexImage2D(__VA_ARGS__)
#define vglMemFree(...)				::VitaD3D8::QueuedGL::MemFree(__VA_ARGS__)
#define vglMemTotal(...)			::VitaD3D8::QueuedGL::MemTotal(__VA_ARGS__)
#define vglSwapBuffers(...)			::VitaD3D8::QueuedGL::SwapBuffers(__VA_ARGS__)
