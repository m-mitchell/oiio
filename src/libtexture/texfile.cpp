/////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2008 Larry Gritz
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// 
// (this is the MIT license)
/////////////////////////////////////////////////////////////////////////////


#include <string>
#include <boost/scoped_ptr.hpp>
#include <boost/tr1/memory.hpp>
using namespace std::tr1;

#include <ImathVec.h>
#include <ImathMatrix.h>
#include <half.h>

#include "dassert.h"
#include "paramtype.h"
#include "varyingref.h"
#include "ustring.h"
#include "hash.h"
#include "thread.h"
#include "fmath.h"
#include "imageio.h"
using namespace OpenImageIO;

#define DLL_EXPORT_PUBLIC /* Because we are implementing TextureSystem */
#include "texture.h"
#undef DLL_EXPORT_PUBLIC

#include "texture_pvt.h"
using namespace OpenImageIO::pvt;


namespace OpenImageIO {


static float default_blur = 0;
static float default_width = 1;
static float default_bias = 0;
static float default_fill = 0;

static TextureOptions defaultTextureOptions(true);  // use special ctr



/// Special private ctr that makes a canonical default TextureOptions.
/// For use internal to libtexture.  Users, don't call this!
TextureOptions::TextureOptions (bool)
    : firstchannel(0), nchannels(1),
      swrap(WrapDefault), twrap(WrapDefault),
      sblur(default_blur), tblur(default_blur),
      swidth(default_width), twidth(default_width),
      bias(default_bias),
      fill(default_fill),
      alpha(NULL),
      stateful(false)
{
    
}



TextureOptions::TextureOptions ()
{
    memcpy (this, &defaultTextureOptions, sizeof(*this));
}



TextureSystem *
TextureSystem::create ()
{
    return new TextureSystemImpl;
}



void
TextureSystem::destroy (TextureSystem * &x)
{
    delete x;
    x = NULL;
}



namespace pvt {   // namespace TextureSystem::pvt


static const char * texture_format_name[] = {
    // MUST match the order of TexFormat
    "unknown", "Plain Texture", "Volume Texture",
    "Shadow", "CubeFace Shadow", "Volume Shadow",
    "LatLong Environment", "CubeFace Environment",
    ""
};



static const char * texture_type_name[] = {
    // MUST match the order of TexFormat
    "unknown", "Plain Texture", "Volume Texture",
    "Shadow", "Shadow", "Shadow",
    "Environment", "Environment",
    ""
};



static const char * wrap_type_name[] = {
    // MUST match the order of TextureOptions::Wrap
    "default", "black", "clamp", "periodic", "mirror",
    ""
};


static TextureOptions::Wrap
decode_wrapmode (const char *name)
{
    for (int i = 0;  i < (int)TextureOptions::WrapLast;  ++i)
        if (! strcmp (name, wrap_type_name[i]))
            return (TextureOptions::Wrap) i;
    return TextureOptions::WrapDefault;
}



static void
parse_wrapmodes (const char *wrapmodes, TextureOptions::Wrap &m_swrap,
                 TextureOptions::Wrap &m_twrap)
{
    char *swrap = (char *) alloca (strlen(wrapmodes)+1);
    const char *twrap;
    int i;
    for (i = 0;  wrapmodes[i] && wrapmodes[i] != ',';  ++i)
        swrap[i] = wrapmodes[i];
    swrap[i] = 0;
    if (wrapmodes[i] == ',')
        twrap = wrapmodes + i+1;
    else twrap = swrap;
    m_swrap = decode_wrapmode (swrap);
    m_twrap = decode_wrapmode (twrap);
}



TextureFile::TextureFile (TextureSystemImpl &texsys, ustring filename)
    : m_filename(filename), m_used(true), m_broken(false),
      m_texformat(TexFormatTexture), 
      m_swrap(TextureOptions::WrapBlack), m_twrap(TextureOptions::WrapBlack),
      m_cubelayout(CubeUnknown), m_y_up(false),
      m_texsys(texsys)
{
    m_spec.clear ();
    open ();
}



TextureFile::~TextureFile ()
{
    release ();
}



void
TextureFile::open ()
{
    if (m_input)         // Already opened
        return;
    if (m_broken)        // Already failed an open -- it's broken
        return;
    
    m_input.reset (ImageInput::create (m_filename.c_str(),
                                       m_texsys.searchpath().c_str()));
    if (! m_input) {
        m_broken = true;
        return;
    }

    ImageIOFormatSpec tempspec;
    if (! m_input->open (m_filename.c_str(), tempspec)) {
        m_broken = true;
        m_input.reset ();
        return;
    }
    m_texsys.incr_open_files ();
    use ();

    // If m_spec has already been filled out, we've opened this file
    // before, read the spec, and filled in all the fields.  So now that
    // we've re-opened it, we're done.
    if (m_spec.size())
        return;

    // From here on, we know that we've opened this file for the very
    // first time.  So read all the MIP levels, fill out all the fields
    // of the TextureFile.
    m_spec.reserve (16);
    int nsubimages = 0;
    do {
        ++nsubimages;
        m_spec.push_back (tempspec);
        // Sanity checks: all levels need the same num channels
        ASSERT (tempspec.nchannels == m_spec[0].nchannels);
    } while (m_input->seek_subimage (nsubimages, tempspec));
    std::cerr << m_filename << " has " << m_spec.size() << " subimages\n";
    ASSERT (nsubimages = m_spec.size());

    const ImageIOFormatSpec &spec (m_spec[0]);
    const ImageIOParameter *p;

    m_texformat = TexFormatTexture;
    p = spec.find_attribute ("textureformat");
    if (p && p->type == PT_STRING && p->nvalues == 1) {
        const char *textureformat = (const char *)p->data();
        for (int i = 0;  i < TexFormatLast;  ++i)
            if (! strcmp (textureformat, texture_format_name[i])) {
                m_texformat = (TexFormat) i;
                break;
            }
    }

    p = spec.find_attribute ("wrapmodes");
    if (p && p->type == PT_STRING && p->nvalues == 1) {
        const char *wrapmodes = (const char *)p->data();
        parse_wrapmodes (wrapmodes, m_swrap, m_twrap);
    }

    m_y_up = false;
    if (m_texformat == TexFormatCubeFaceEnv) {
        if (! strcmp (m_input->format_name(), "openexr"))
            m_y_up = true;
        int w = std::max (spec.full_width, spec.tile_width);
        int h = std::max (spec.full_height, spec.tile_height);
        if (spec.width == 3*w && spec.height == 2*h)
            m_cubelayout = CubeThreeByTwo;
        else if (spec.width == w && spec.height == 6*h)
            m_cubelayout = CubeOneBySix;
        else
            m_cubelayout = CubeLast;
    }

    Imath::M44f c2w;
    m_texsys.get_commontoworld (c2w);
    p = spec.find_attribute ("worldtocamera");
    if (p && p->type == PT_MATRIX && p->nvalues == 1) {
        const Imath::M44f *m = (const Imath::M44f *)p->data();
        m_Mlocal = c2w * (*m);
    }
    p = spec.find_attribute ("worldtoscreen");
    if (p && p->type == PT_MATRIX && p->nvalues == 1) {
        const Imath::M44f *m = (const Imath::M44f *)p->data();
        m_Mproj = c2w * (*m);
    }
    // FIXME -- compute Mtex, Mras
}



bool
TextureFile::read_tile (int level, int x, int y, int z,
                        ParamBaseType format, void *data)
{
    open ();
    ImageIOFormatSpec tmp;
    if (m_input->current_subimage() != level)
        m_input->seek_subimage (level, tmp);
    return m_input->read_tile (x, y, z, format, data);
}



void
TextureFile::release ()
{
    if (m_used) {
        m_used = false;
    } else if (opened()) {
        m_input->close ();
        m_input.reset ();
        m_used = false;
        m_texsys.decr_open_files ();
    }
}



Tile::Tile (const TileID &id)
    : m_id (id), m_valid(true), m_used(true)
{
    TextureFile &texfile = m_id.texfile ();
    const ImageIOFormatSpec &spec (texfile.spec());
    ParamBaseType peltype = PT_FLOAT;
    // FIXME -- read 8-bit directly if that's native
    m_texels.resize (spec.tile_pixels () * spec.nchannels * typesize(peltype));
    if (! texfile.read_tile (m_id.level(), m_id.x(), m_id.y(), m_id.z(),
                             peltype, &m_texels[0])) {
        std::cerr << "(1) error reading tile\n";
    }
    // FIXME -- for shadow, fill in mindepth, maxdepth
}



TextureSystemImpl::TextureSystemImpl ()
    : m_open_files(0)
{
    init ();
}



TextureSystemImpl::~TextureSystemImpl ()
{
}



void
TextureSystemImpl::init ()
{
    max_open_files (100);
    max_memory_MB (50);
    m_Mw2c.makeIdentity();
}



TextureFileRef
TextureSystemImpl::find_texturefile (ustring filename)
{
    lock_guard guard (m_texturefiles_mutex);

    FilenameMap::iterator found = m_texturefiles.find (filename);
    TextureFileRef tf;
    if (found == m_texturefiles.end()) {
        // We don't already have this file in the texture list.  Try to
        // open it and create a record.
        check_max_files ();
        tf.reset (new TextureFile (*this, filename));
        m_texturefiles[filename] = tf;
    } else {
        tf = found->second;
    }

    tf->use ();
    return tf;
}



void
TextureSystemImpl::check_max_files ()
{
    std::cerr << "open files " << m_open_files << ", max = " << m_max_open_files << "\n";
    while (m_open_files >= m_max_open_files) {
        if (m_file_sweep == m_texturefiles.end())
            m_file_sweep = m_texturefiles.begin();
        ASSERT (m_file_sweep != m_texturefiles.end());
        m_file_sweep->second->release ();  // May reduce m_open_files
    }
}



TileRef
TextureSystemImpl::find_tile (const TileID &id)
{
    DASSERT (id.texfile() != NULL);
    lock_guard guard (m_texturefiles_mutex);
    TileCache::iterator found = tilecache.find (id);
    TileRef tile;
    if (found != tilecache.end()) {
        tile = found->second;
    } else {
        tile.reset (new Tile (id));
        tilecache[id] = tile;
    }
    DASSERT (id == tile->id() && !memcmp(&id, &tile->id(), sizeof(TileID)));
    return tile;
}



bool
TextureSystemImpl::gettextureinfo (ustring filename, ustring dataname,
                                   ParamType datatype, void *data)
{
    std::cerr << "gettextureinfo \"" << filename << "\"\n";

    TextureFileRef texfile = find_texturefile (filename);
    if (! texfile) {
        std::cerr << "   NOT FOUND\n";
        return false;
    }
    if (texfile->broken()) {
        std::cerr << "    Invalid file\n";
        return false;
    }
    const ImageIOFormatSpec &spec (texfile->spec());
    if (dataname == "resolution" && datatype==ParamType(PT_INT,2)) {
        int *d = (int *)data;
        d[0] = spec.width;
        d[1] = spec.height;
        return true;
    }
    if (dataname == "texturetype" && datatype==ParamType(PT_STRING)) {
        ustring s (texture_type_name[(int)texfile->textureformat()]);
        *(const char **)data = s.c_str();
        return true;
    }
    if (dataname == "textureformat" && datatype==ParamType(PT_STRING)) {
        ustring s (texture_format_name[(int)texfile->textureformat()]);
        *(const char **)data = s.c_str();
        return true;
    }
    if (dataname == "channels" && datatype==ParamType(PT_INT)) {
        *(int *)data = spec.nchannels;
        return true;
    }
    if (dataname == "channels" && datatype==ParamType(PT_FLOAT)) {
        *(float *)data = spec.nchannels;
        return true;
    }
    // FIXME - "viewingmatrix"
    // FIXME - "projectionmatrix"

    // general case
    const ImageIOParameter *p = spec.find_attribute (dataname.string());
    if (p && p->nvalues == datatype.arraylen) {
        // First test for exact type match
        if (p->type == datatype.basetype) {
            memcpy (data, p->data(), datatype.datasize());
            return true;
        }
        // If the real data is int but user asks for float, translate it
        if (p->type == PT_FLOAT && datatype.basetype == PT_INT) {
            for (int i = 0;  i < p->nvalues;  ++i)
                ((float *)data)[i] = ((int *)p->data())[i];
            return true;
        }
    }

    return false;
}



void
TextureSystemImpl::texture (ustring filename, TextureOptions &options,
                            Runflag *runflags, int firstactive, int lastactive,
                            VaryingRef<float> s, VaryingRef<float> t,
                            VaryingRef<float> dsdx, VaryingRef<float> dtdx,
                            VaryingRef<float> dsdy, VaryingRef<float> dtdy,
                            float *result)
{
    // FIXME - should we be keeping stats, times?

    TextureFileRef texturefile = find_texturefile (filename);
    if (! texturefile  ||  texturefile->broken()) {
        std::cerr << "   TEXTURE NOT FOUND " << filename << "\n";
        for (int i = firstactive;  i <= lastactive;  ++i) {
            if (runflags[i]) {
                for (int c = 0;  c < options.nchannels;  ++c)
                    result[c] = options.fill;
                if (options.alpha)
                    options.alpha[i] = options.fill;
            }
            result += options.nchannels;
        }
        return ;
    }

    // If options indicate default wrap modes, use the ones in the file
    if (options.swrap == TextureOptions::WrapDefault)
        options.swrap = texturefile->swrap();
    if (options.twrap == TextureOptions::WrapDefault)
        options.twrap = texturefile->twrap();

    int actualchannels = Imath::clamp (texturefile->spec().nchannels - options.firstchannel, 0, options.nchannels);
    options.actualchannels = actualchannels;

    // Fill channels requested but not in the file
    if (options.actualchannels < options.nchannels) {
        for (int i = firstactive;  i <= lastactive;  ++i) {
            if (runflags[i]) {
                float fill = options.fill[i];
                for (int c = options.actualchannels; c < options.nchannels; ++c)
                    result[i*options.nchannels+c] = fill;
            }
        }
    }
    // Fill alpha if requested and it's not in the file
    if (options.alpha && options.actualchannels+1 < options.nchannels) {
        for (int i = firstactive;  i <= lastactive;  ++i)
            options.alpha[i] = options.fill[i];
        options.alpha.init (NULL);  // No need for texture_lookup to care
    }
    // Early out if all channels were beyond the highest in the file
    if (options.actualchannels < 1)
        return;

    // FIXME - allow multiple filtered texture implementations

    // Loop over all the points that are active (as given in the
    // runflags), and for each, call texture_lookup.  The separation of
    // power here is that all possible work that can be done for all
    // "grid points" at once should be done in this function, outside
    // the loop, and all the work inside texture_lookup should be work
    // that MUST be redone for each individual texture lookup point.
    for (int i = firstactive;  i <= lastactive;  ++i) {
        if (runflags[i]) {
            texture_lookup (*texturefile, options, i,
#if 0
                            s[i], t[i],
                            dsdx[i] * swidth[i] + sblur[i],
                            dtdx[i] * twidth[i] + tblur[i],
                            dsdy[i] * swidth[i] + sblur[i],
                            dtdy[i] * twidth[i] + tblur[i],
                            result + i * options.nchannels
#else
                            s, t, dsdx, dtdx, dsdy, dtdy, result + i * options.nchannels
#endif
                );
        }
    }
}



void
TextureSystemImpl::texture_lookup (TextureFile &texturefile,
                            TextureOptions &options,
                            int index,
                            VaryingRef<float> _s, VaryingRef<float> _t,
                            VaryingRef<float> _dsdx, VaryingRef<float> _dtdx,
                            VaryingRef<float> _dsdy, VaryingRef<float> _dtdy,
                            float *result)
{
    // N.B. If any computations within this function are identical for
    // all texture lookups in this batch, those computations should be
    // hoisted up to the calling function, texture().
    float dsdx = _dsdx ? (_dsdx[index] * options.swidth[index] + options.sblur[index]) : 0;
    float dtdx = _dtdx ? (_dtdx[index] * options.twidth[index] + options.tblur[index]) : 0;
    float dsdy = _dsdy ? (_dsdy[index] * options.swidth[index] + options.sblur[index]) : 0;
    float dtdy = _dtdy ? (_dtdy[index] * options.twidth[index] + options.tblur[index]) : 0;
    result += index * options.nchannels;
    result[0] = _s[index];
    result[1] = _t[index];

    // Very primitive -- unfiltered, uninterpolated lookup
    int level = 0;
    const ImageIOFormatSpec &spec (texturefile.spec (level));
    // As passed in, (s,t) map the texture to (0,1)
    float s = _s[index] * spec.width;
    float t = _t[index] * spec.height;
    // Now (s,t) map the texture to (0,res)
    s -= 0.5f;
    t -= 0.5f;
    int sint, tint;
    float sfrac = floorfrac (s, &sint);
    float tfrac = floorfrac (t, &tint);
    // Now (xint,yint) are the integer coordinates of the texel to the
    // immediate "upper left" of the lookup point, and (xfrac,yfrac) are
    // the amount that the lookup point is actually offset from the
    // texel center (with (1,1) being all the way to the next texel down
    // and to the right).
    
    // Wrap not implemented yet.  Just ignore lookups outside the texture.
    if (sint < 0 || sint >= spec.width || tint < 0 || tint >= spec.height) {
        result[0] = 1;
        return;
    }

    int tilewidthmask = spec.tile_width - 1;
    int tileheightmask = spec.tile_height - 1;
    int tile_s = sint & tilewidthmask;
    int tile_t = tint & tileheightmask;
    TileID id (texturefile, 0 /* always level 0 for now */, 
               sint - tile_s, tint - tile_t, 0);
    TileRef tile = find_tile (id);
    if (! tile) {
        result[0] = 0.5;
        return;
    }
    DASSERT (tile->id() == id);

    // FIXME -- float only for now
    int offset = (tile_t * spec.tile_width + tile_s);
    DASSERT (offset < spec.tile_pixels());
    const float *data = tile->data() + offset * spec.nchannels;
    for (int c = 0;  c < options.actualchannels;  ++c)
        result[c] = data[options.firstchannel+c];
    if (options.alpha)
        options.alpha[index] = data[options.firstchannel+options.actualchannels];
#if 0
    // Debug: R,G show the coords within a tile
    result[0] = (float)tile_s / spec.tile_width;
    result[1] = (float)tile_t / spec.tile_height;
#endif
#if 0
    // Debug: R,G show the tile id
    result[0] = (float) (sint) / spec.width;
    result[1] = (float) (tint) / spec.height;
//    result[2] = (float)offset / spec.tile_pixels();
#endif
#if 0
    // Debug: R,G show the tile id
//    result[0] = (float) (sint & (~tilewidthmask)) / spec.width;
    result[1] = (float) (tint & (~tileheightmask)) / spec.height;
//    result[2] = (float)offset / spec.tile_pixels();
#endif
}


};  // end namespace OpenImageIO::pvt
};  // end namespace OpenImageIO
