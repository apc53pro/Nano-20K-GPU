#include "nano_gpu/reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nano_gpu;

namespace {

int cases_run = 0;
int cases_failed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void expect_throws(Function&& function, const std::string& message) {
    bool threw = false;
    try { function(); }
    catch (const std::exception&) { threw = true; }
    expect(threw, message);
}

template<class Function>
void run_case(const std::string& name, Function&& function) {
    ++cases_run;
    try { function(); }
    catch (const std::exception& error) {
        ++cases_failed;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

std::size_t pixel_index(int x, int y) {
    return static_cast<std::size_t>(y) * screen_width + x;
}

Triangle require_triangle(std::array<ScreenVertex,3> vertices, const RenderState& state = {}) {
    auto triangle = setup_triangle(vertices, state);
    expect(triangle.has_value(), "expected a nondegenerate triangle");
    return *triangle;
}

Triangle screen_triangle(std::array<Point,3> positions, const RenderState& state = {},
                         long double normalized_depth = 0.5L) {
    std::array<ScreenVertex,3> vertices{};
    for (std::size_t i=0; i<vertices.size(); ++i) {
        vertices[i].x = positions[i].x;
        vertices[i].y = positions[i].y;
        vertices[i].depth = normalized_depth;
    }
    return require_triangle(vertices,state);
}

std::vector<Point> coverage_points(const Triangle& triangle) {
    std::vector<Point> points;
    for (int y=triangle.bounds.y0; y<triangle.bounds.y1; ++y)
        for (int x=triangle.bounds.x0; x<triangle.bounds.x1; ++x)
            if (covered(triangle,x,y)) points.push_back({x,y});
    return points;
}

std::set<std::pair<int,int>> point_set(const std::vector<Point>& points) {
    std::set<std::pair<int,int>> result;
    for (auto p : points) result.emplace(p.x,p.y);
    return result;
}

bool same_plane(Plane a, Plane b) {
    return a.seed == b.seed && a.step_x == b.step_x && a.step_y == b.step_y;
}

std::shared_ptr<Image> solid_texture(std::uint16_t value, int width=1, int height=1) {
    auto image = std::make_shared<Image>(width,height);
    for (int y=0; y<height; ++y)
        for (int x=0; x<width; ++x) image->at(x,y)=value;
    return image;
}

Rect rectangle(Bounds bounds, Color color, std::uint32_t depth=0, bool use_depth=false) {
    Rect rect;
    rect.bounds=bounds;
    rect.state = use_depth ? RenderState{} : overlay_state(color);
    rect.state.color=color;
    rect.depth=depth;
    return rect;
}

void expect_buffers_equal(const Renderer& a, const Renderer& b, const std::string& label) {
    expect(a.color(0)==b.color(0), label+": color 0 differs");
    expect(a.color(1)==b.color(1), label+": color 1 differs");
    expect(a.depth()==b.depth(), label+": depth differs");
    expect(a.statistics().covered_fragments==b.statistics().covered_fragments,
           label+": covered fragment count differs");
    expect(a.statistics().color_writes==b.statistics().color_writes,
           label+": committed color count differs");
    expect(a.statistics().depth_writes==b.statistics().depth_writes,
           label+": committed depth count differs");
}

void expect_modes_equal(const Pass& pass) {
    Renderer tiled, reverse, monolithic;
    tiled.render(pass,RenderMode::Tiled);
    reverse.render(pass,RenderMode::TiledReverse);
    monolithic.render(pass,RenderMode::Monolithic);
    expect_buffers_equal(tiled,reverse,"forward/reverse tile order");
    expect_buffers_equal(tiled,monolithic,"tiled/monolithic");
}

#include "golden_vectors.hpp"

void numeric_tests() {
    run_case("signed round-to-nearest-even ties", [] {
        const std::array<long double,10> input{{0.5L,1.5L,2.5L,3.5L,-0.5L,-1.5L,-2.5L,-3.5L,1.499L,-1.501L}};
        const std::array<std::int64_t,10> expected{{0,2,2,4,0,-2,-2,-4,1,-2}};
        for (std::size_t i=0;i<input.size();++i) expect(round_even(input[i])==expected[i],"signed RNE result");
        expect(divide_round_even(5,2)==2 && divide_round_even(7,2)==4,"positive rational ties");
        expect(divide_round_even(-5,2)==-2 && divide_round_even(-7,2)==-4,"negative rational ties");
        expect(divide_round_even(7,-2)==-4 && divide_round_even(-7,-2)==4,"negative denominator");
    });
    run_case("numeric invalid inputs and overflow", [] {
        expect_throws([] { round_even(std::numeric_limits<long double>::infinity()); },"infinite RNE input");
        expect_throws([] { round_even(std::numeric_limits<long double>::quiet_NaN()); },"NaN RNE input");
        expect_throws([] { round_even(1.0e30L); },"RNE overflow");
        expect_throws([] { divide_round_even(1,0); },"division by zero");
        expect_throws([] { divide_round_even(std::numeric_limits<std::int64_t>::min(),-1); },"rational overflow");
        expect_throws([] { Plane{std::numeric_limits<std::int64_t>::max(),1,0}.at(1,0); },"plane addition overflow");
    });
    run_case("signed UV floor and color/depth conversion", [] {
        expect(floor_q16(-1)==-1 && floor_q16(-65536)==-1 && floor_q16(-65537)==-2,"negative UV floor");
        expect(floor_q16(65535)==0 && floor_q16(65536)==1,"positive UV floor");
        expect(depth_code(-1)==0 && depth_code(static_cast<std::int64_t>(depth_max)*65536)==depth_max,"D24 clamp");
        expect(depth_code(32768)==0 && depth_code(98304)==2,"D24 tied rounding");
        expect(color_code(-65536)==0 && color_code(256LL*65536)==255,"RGB channel clamp");
    });
    run_case("Q4 snapping uses ties-even", [] {
        std::array<ScreenVertex,3> v{};
        v[0].x=0.03125L;v[0].y=0.03125L;
        v[1].x=4.09375L;v[1].y=0;
        v[2].x=0;v[2].y=4;
        const auto t=require_triangle(v);
        expect(t.snapped[0]==Point{0,0},"half-subpixel snaps even down");
        expect(t.snapped[1]==Point{66,0},"half-subpixel snaps even up");
    });
    run_case("winding normalization preserves attributes", [] {
        std::array<ScreenVertex,3> v{};
        v[0].x=30;v[0].y=20;v[0].depth=0.2L;v[0].r=200;
        v[1].x=90;v[1].y=30;v[1].depth=0.4L;v[1].g=120;
        v[2].x=40;v[2].y=80;v[2].depth=0.7L;v[2].b=60;
        RenderState s;s.shade=ShadeMode::Gouraud;
        const auto forward=require_triangle(v,s);
        std::swap(v[1],v[2]);
        const auto reverse=require_triangle(v,s);
        expect(forward.snapped==reverse.snapped,"canonical snapped winding");
        expect(same_plane(forward.z,reverse.z) && same_plane(forward.r,reverse.r) &&
               same_plane(forward.g,reverse.g) && same_plane(forward.b,reverse.b),"canonical attribute planes");
    });
    run_case("shared edges for every Q4 fractional translation", [] {
        for (int fy=0;fy<16;++fy) for (int fx=0;fx<16;++fx) {
            const long double left=fx/16.0L,top=fy/16.0L;
            std::array<ScreenVertex,3> a{},b{};
            a[0].x=left;a[0].y=top;a[1].x=left+2;a[1].y=top;a[2].x=left;a[2].y=top+2;
            b[0]=a[1];b[1].x=left+2;b[1].y=top+2;b[2]=a[2];
            const auto ta=require_triangle(a),tb=require_triangle(b);
            for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
                const bool inside=x+0.5L>=left && x+0.5L<left+2 && y+0.5L>=top && y+0.5L<top+2;
                const int owners=static_cast<int>(covered(ta,x,y))+static_cast<int>(covered(tb,x,y));
                expect(owners==static_cast<int>(inside),"square has exact one-owner coverage");
            }
        }
    });
    run_case("scissor is half-open across a tile boundary", [] {
        RenderState state=overlay_state();state.scissor={32,31,34,34};
        const auto t=screen_triangle({{{30,30},{36,30},{30,36}}},state);
        const auto points=point_set(coverage_points(t));
        const std::set<std::pair<int,int>> expected{{32,31},{33,31},{32,32}};
        expect(points==expected,"scissored triangle coverage");
    });
    run_case("degenerate and invalid setup input", [] {
        std::array<ScreenVertex,3> v{};
        v[1].x=1;v[1].y=1;v[2].x=2;v[2].y=2;
        expect(!setup_triangle(v).has_value(),"zero-area triangle is culled");
        v[1].x=4;v[1].y=0;v[2].x=0;v[2].y=4;
        v[0].x=std::numeric_limits<long double>::quiet_NaN();
        expect_throws([&] { setup_triangle(v); },"NaN screen coordinate rejected");
        v[0].x=-1;
        expect_throws([&] { setup_triangle(v); },"setup requires viewport-clipped coordinates");
    });
    run_case("perspective supported range is enforced", [] {
        std::array<ScreenVertex,3> v{};
        v[1].x=4;v[2].y=4;
        RenderState state;state.shade=ShadeMode::TextureReplace;state.texture=solid_texture(0xffffu);
        v[1].w=64;
        expect(setup_triangle(v,state).has_value(),"W ratio 64 supported");
        v[1].w=65;
        expect_throws([&] { setup_triangle(v,state); },"W ratio beyond profile rejected");
        v[1].w=1;v[0].w=0;
        expect_throws([&] { setup_triangle(v,state); },"nonpositive W rejected");
        v[0].w=1;v[0].u=1024.25L;
        expect_throws([&] { setup_triangle(v,state); },"UV outside supported range rejected");
    });
    run_case("homogeneous clipping rejects and clips near plane", [] {
        std::array<ClipVertex,3> v{};
        v[0].x=-0.5L;v[0].y=-0.5L;v[0].z=-0.5L;
        v[1].x=0.5L;v[1].y=-0.5L;v[1].z=0.5L;
        v[2].x=0;v[2].y=0.5L;v[2].z=0.5L;
        const auto clipped=clip_project_triangle(v);
        expect(clipped.size()==2,"near clipping creates a quadrilateral fan");
        for (const auto& triangle:clipped) for (const auto& p:triangle)
            expect(p.x>=0 && p.x<=640 && p.y>=0 && p.y<=480 && p.depth>=0 && p.depth<=1 && p.w>0,"clipped projection domain");
        for (auto& p:v) p.x=-2;
        expect(clip_project_triangle(v).empty(),"fully outside geometry rejected");
    });
    run_case("shared frustum clipping preserves a watertight rectangle", [] {
        const std::array<ClipVertex,4> corners{{
            {-2,-0.5L,0.5L,1},{0,-0.5L,0.5L,1},{0,0.5L,0.5L,1},{-2,0.5L,0.5L,1}}};
        Pass pass;
        for (const auto& indices:std::array<std::array<int,3>,2>{{{{0,1,2}},{{0,2,3}}}}) {
            const std::array<ClipVertex,3> input{{corners[indices[0]],corners[indices[1]],corners[indices[2]]}};
            for (const auto& projected:clip_project_triangle(input))
                if (auto t=setup_triangle(projected,overlay_state())) pass.primitives.emplace_back(*t);
        }
        Renderer renderer;renderer.render(pass);
        expect(renderer.statistics().color_writes==320u*240u,"clipped rectangle has no shared-edge double writes");
        for (int y=0;y<screen_height;++y) for (int x=0;x<screen_width;++x)
            expect(renderer.color(1)[pixel_index(x,y)]==((x<320 && y>=120 && y<360)?0xffffu:0u),"clipped rectangle extent");
    });
    run_case("global line ties and reversal", [] {
        const std::vector<Point> expected{{0,0},{1,1},{2,1}};
        expect(line_pixels({0,0},{2,1})==expected,"line tie selects defined sample");
        expect(line_pixels({2,1},{0,0})==expected,"line tie reversal");
        for (const auto& end:std::array<Point,6>{{{60,40},{40,60},{0,40},{40,0},{20,60},{60,20}}})
            expect(line_pixels({30,30},end)==line_pixels(end,{30,30}),"all line octants reverse identically");
        expect(line_pixels({5,5},{5,5})==std::vector<Point>{{5,5}},"degenerate line includes endpoint once");
    });
}

void shading_tests() {
    run_case("image storage bounds and texture compatibility", [] {
        expect_throws([] { Image image(0,1); },"zero-size image rejected");
        expect_throws([] { Image image(2,1,2); },"short pitch rejected");
        Image image(3,2);
        expect(!image.texture_compatible(),"non-POT image is not a texture");
        expect_throws([&] { image.at(-1,0); },"negative image coordinate rejected");
        expect_throws([&] { image.at(3,0); },"right image bound rejected");
    });
    run_case("texture clamp and negative repeat", [] {
        Image image(4,4);
        for (int y=0;y<4;++y) for (int x=0;x<4;++x) image.at(x,y)=static_cast<std::uint16_t>(4*y+x+1);
        expect(image.sample(-1,-1,AddressMode::Repeat,AddressMode::Repeat)==16,"negative repeat wraps both axes");
        expect(image.sample(-5,4,AddressMode::Repeat,AddressMode::Repeat)==4,"repeat uses mathematical modulo");
        expect(image.sample(-1,9,AddressMode::Clamp,AddressMode::Clamp)==13,"clamp is independent per axis");
    });
    run_case("every triangle shading mode has defined output", [] {
        std::array<ScreenVertex,3> vertices{};
        vertices[1].x=4;vertices[2].y=4;
        vertices[0].r=255;vertices[0].g=0;vertices[0].b=0;
        vertices[1].r=0;vertices[1].g=255;vertices[1].b=0;
        vertices[2].r=0;vertices[2].g=0;vertices[2].b=255;
        const std::array<ShadeMode,5> modes{{ShadeMode::Flat,ShadeMode::Gouraud,ShadeMode::TextureReplace,
            ShadeMode::TextureFlatModulate,ShadeMode::TextureGouraudModulate}};
        const std::array<std::uint16_t,5> expected{{0x8204u,0xb904u,0xffffu,0x8204u,0xb904u}};
        for (std::size_t i=0;i<modes.size();++i) {
            RenderState state=overlay_state(Color{128,64,32});state.shade=modes[i];
            state.texture=solid_texture(0xffffu);
            Pass pass;pass.primitives.emplace_back(require_triangle(vertices,state));
            Renderer renderer;renderer.render(pass);
            expect(renderer.color(1)[0]==expected[i],"flat/Gouraud/texture/modulation mode output");
        }
    });
    run_case("texture replacement ignores supplied flat color", [] {
        RenderState state=overlay_state(Color{0,0,0});state.shade=ShadeMode::TextureReplace;
        state.texture=solid_texture(0x1234u);
        Pass pass;pass.primitives.emplace_back(screen_triangle({{{0,0},{2,0},{0,2}}},state));
        Renderer renderer;renderer.render(pass);
        expect(renderer.color(1)[0]==0x1234u,"raw RGB565 replacement is preserved");
    });
    run_case("negative fractional UV floors before addressing", [] {
        auto texture=solid_texture(0xf800u,2,2);texture->at(1,0)=0x07e0u;
        std::array<ScreenVertex,3> vertices{};vertices[1].x=2;vertices[2].y=2;
        for (auto& v:vertices) {v.u=-1.0L/65536;v.v=0.5L;}
        for (auto address:std::array<AddressMode,2>{{AddressMode::Clamp,AddressMode::Repeat}}) {
            RenderState state=overlay_state();state.shade=ShadeMode::TextureReplace;
            state.texture=texture;state.address_u=address;
            Pass pass;pass.primitives.emplace_back(require_triangle(vertices,state));
            Renderer renderer;renderer.render(pass);
            expect(renderer.color(1)[0]==(address==AddressMode::Clamp?0xf800u:0x07e0u),"negative fractional sample address");
        }
    });
    run_case("clipped sprites retain source origin across tiles", [] {
        auto image=solid_texture(0,4,4);
        for (int y=0;y<4;++y) for (int x=0;x<4;++x) image->at(x,y)=static_cast<std::uint16_t>(4*y+x+1);
        Sprite sprite;sprite.bounds={-1,31,3,35};sprite.state.texture=image;
        Pass pass;pass.primitives.emplace_back(sprite);
        Renderer renderer;renderer.render(pass);
        expect(renderer.color(1)[pixel_index(0,31)]==image->at(1,0),"left clipping advances source");
        expect(renderer.color(1)[pixel_index(2,34)]==image->at(3,3),"mapping survives tile crossing");
        expect(renderer.color(1)[pixel_index(3,31)]==0,"sprite right edge is exclusive");
        expect_modes_equal(pass);
    });
    run_case("keying happens before modulation and depth commit", [] {
        RenderState state;state.shade=ShadeMode::TextureFlatModulate;
        state.texture=solid_texture(0xf81fu);state.color_key=0xf81fu;state.color={0,0,0};
        Pass pass;pass.primitives.emplace_back(screen_triangle({{{0,0},{2,0},{0,2}}},state,0.1L));
        pass.primitives.emplace_back(rectangle({0,0,1,1},{0,255,0},0x800000u,true));
        Renderer renderer;renderer.render(pass);
        expect(renderer.color(1)[0]==0x07e0u && renderer.depth()[0]==0x800000u,"raw keyed near sample cannot hide farther primitive");
        expect(renderer.statistics().keyed_rejected==1,"raw key was recognized before color became black");
    });
    run_case("depth-only keyed triangles still sample and reject", [] {
        RenderState state;state.shade=ShadeMode::TextureReplace;state.color_write=false;
        state.texture=solid_texture(0xf81fu);state.color_key=0xf81fu;
        Pass pass;pass.primitives.emplace_back(screen_triangle({{{0,0},{2,0},{0,2}}},state,0.1L));
        pass.primitives.emplace_back(rectangle({0,0,1,1},{255,0,0},0x800000u,true));
        Renderer renderer;renderer.render(pass);
        expect(renderer.depth()[0]==0x800000u && renderer.color(1)[0]==0xf800u,"depth-only key rejects the whole fragment");
        expect(renderer.statistics().texture_samples>=1 && renderer.statistics().keyed_rejected==1,"keyed depth-only draw sampled texture");
    });
}

void depth_and_pass_tests() {
    run_case("depth functions define equality and ordering", [] {
        for (auto func:std::array<DepthFunc,3>{{DepthFunc::Less,DepthFunc::LessEqual,DepthFunc::Always}}) {
            Pass pass;
            auto first=rectangle({0,0,1,1},{255,0,0},100,true);
            auto equal=rectangle({0,0,1,1},{0,255,0},100,true);equal.state.depth_func=func;
            pass.primitives={first,equal};
            Renderer renderer;renderer.render(pass);
            expect(renderer.color(1)[0]==(func==DepthFunc::Less?0xf800u:0x07e0u),"depth equality rule");
            if (func==DepthFunc::Always) {
                auto farther=rectangle({0,0,1,1},{0,0,255},200,true);farther.state.depth_func=func;
                pass.primitives.push_back(farther);renderer.render(pass);
                expect(renderer.depth()[0]==200 && renderer.color(1)[0]==0x001fu,"ALWAYS admits farther sample");
            }
        }
    });
    run_case("color and depth masks are independent", [] {
        Pass pass;pass.clear_color=0x001fu;pass.clear_depth=50;
        auto depth_only=rectangle({0,0,1,1},{255,0,0},25,true);depth_only.state.color_write=false;
        auto overlay=rectangle({0,0,1,1},{0,255,0},100,true);overlay.state.depth_test=false;overlay.state.depth_write=false;
        pass.primitives={depth_only,overlay};
        Renderer renderer;renderer.render(pass);
        expect(renderer.color(1)[0]==0x07e0u && renderer.depth()[0]==25,"overlay preserves depth-only result");
        expect(renderer.statistics().color_writes==1 && renderer.statistics().depth_writes==1,"masked commit counts");
    });
    run_case("disabled depth test can still write depth", [] {
        Pass pass;pass.clear_depth=0;
        auto rect=rectangle({0,0,1,1},{255,255,255},100,true);rect.state.depth_test=false;
        pass.primitives.emplace_back(rect);
        Renderer renderer;renderer.render(pass);
        expect(renderer.depth()[0]==100 && renderer.color(1)[0]==0xffffu,"independent depth test/write enable");
    });
    run_case("zero-write primitives are valid no-ops", [] {
        Pass pass;pass.clear_color=0x1234u;pass.clear_depth=100;
        auto rect=rectangle({0,0,10,10},{255,255,255},0,true);
        rect.state.color_write=false;rect.state.depth_write=false;pass.primitives.emplace_back(rect);
        Renderer renderer;renderer.render(pass);
        expect(renderer.color(1)[0]==0x1234u && renderer.depth()[0]==100,"both attachments preserved");
        expect(renderer.statistics().color_writes==0 && renderer.statistics().depth_writes==0,"no commits");
    });
    run_case("empty bins still CLEAR entire attachments", [] {
        Renderer renderer;Pass pass;pass.clear_color=0x4567u;pass.clear_depth=123;
        renderer.render(pass);
        expect(std::all_of(renderer.color(1).begin(),renderer.color(1).end(),[](auto p){return p==0x4567u;}),"all color tiles cleared");
        expect(std::all_of(renderer.depth().begin(),renderer.depth().end(),[](auto p){return p==123;}),"all depth tiles cleared");
        expect(renderer.statistics().tiles_processed==tile_count,"empty tiled pass visits 300 tiles");
    });
    run_case("LOAD preserves and independent CLEAR replaces", [] {
        Renderer renderer;Pass first;first.clear_color=0x1234u;first.clear_depth=123;
        first.primitives.emplace_back(rectangle({31,31,33,33},{255,0,0},50,true));renderer.render(first);
        const auto saved=renderer.color(1);const auto saved_depth=renderer.depth();
        Pass second;second.color_load=LoadOp::Load;second.depth_load=LoadOp::Load;renderer.render(second);
        expect(renderer.color(1)==saved && renderer.depth()==saved_depth,"empty LOAD is preservation");
        second.color_load=LoadOp::Clear;second.clear_color=0x07e0u;renderer.render(second);
        expect(renderer.color(1)[pixel_index(31,31)]==0x07e0u && renderer.depth()==saved_depth,"color CLEAR with depth LOAD");
        second.color_load=LoadOp::Load;second.depth_load=LoadOp::Clear;second.clear_depth=999;renderer.render(second);
        expect(renderer.color(1)[pixel_index(31,31)]==0x07e0u && renderer.depth()[pixel_index(31,31)]==999,"color LOAD with depth CLEAR");
    });
    run_case("bin lists preserve each submission occurrence", [] {
        auto rect=rectangle({31,31,33,33},{255,255,255});
        const std::vector<Primitive> primitives{rect,rect};const auto bins=bin_primitives(primitives);
        for (int id:std::array<int,4>{{0,1,20,21}})
            expect(bins[id]==std::vector<std::size_t>({0,1}),"duplicate submissions retain order");
        for (int id=0;id<tile_count;++id)
            if (id!=0 && id!=1 && id!=20 && id!=21) expect(bins[id].empty(),"unrelated bins are empty");
    });
}

void copy_and_ownership_tests() {
    run_case("initial attachments and pending presentation ownership", [] {
        Renderer renderer;
        expect(renderer.front_id()==0 && renderer.back_id()==1,"initial buffer identities");
        expect(renderer.front_color()[0]==0 && renderer.color(1)[0]==0 && renderer.depth()[0]==depth_max,"initial black and far depth");
        Pass pass;pass.clear_color=0xf800u;pass.clear_depth=123;renderer.render(pass);
        renderer.queue_present(1);
        expect(renderer.front_id()==0 && renderer.front_color()[0]==0 && renderer.presentation_pending(),"presentation waits for vertical blank");
        expect_throws([&] { renderer.render(pass); },"sealed back is not writable");
        expect_throws([&] { renderer.copy_rect(0,1,{0,0},{0,0},1,1); },"sealed copy destination rejected");
        renderer.vertical_blank();
        expect(renderer.front_id()==1 && renderer.front_color()[0]==0xf800u && !renderer.presentation_pending(),"vertical blank swaps front");
        expect(renderer.depth()[0]==123,"shared depth does not swap");
        Pass old_front;old_front.target=0;old_front.color_load=LoadOp::Load;old_front.depth_load=LoadOp::Load;renderer.render(old_front);
        expect(renderer.color(0)[0]==0 && renderer.depth()[0]==123,"old front color and latest shared depth are distinct histories");
    });
    run_case("front writes and invalid physical IDs are rejected", [] {
        Renderer renderer;Pass pass;pass.target=0;
        expect_throws([&] { renderer.render(pass); },"displayed buffer render rejected");
        expect_throws([&] { renderer.queue_present(0); },"presenting current front rejected");
        expect_throws([&] { renderer.copy_rect(1,0,{0,0},{0,0},1,1); },"front destination copy rejected");
        pass.target=2;
        expect_throws([&] { renderer.render(pass); },"out-of-range color ID rejected");
    });
    run_case("abort cancels presentation and preserves completed work", [] {
        Renderer renderer;Pass pass;pass.clear_color=0x1234u;pass.clear_depth=789;renderer.render(pass);
        const auto color=renderer.color(1);const auto depth=renderer.depth();
        renderer.queue_present(1);renderer.abort();
        expect(!renderer.presentation_pending() && renderer.front_id()==0,"abort cancels unswapped present");
        expect(renderer.color(1)==color && renderer.depth()==depth && renderer.color_valid(1) && renderer.depth_valid(),"completed state survives synchronous abort");
        renderer.vertical_blank();expect(renderer.front_id()==0,"cancelled presentation cannot later swap");
    });
    run_case("reset reinitializes attachments and ownership", [] {
        Renderer renderer;Pass pass;pass.clear_color=0xf800u;renderer.render(pass);renderer.queue_present(1);renderer.vertical_blank();
        renderer.reset();
        expect(renderer.front_id()==0 && !renderer.presentation_pending(),"reset ownership");
        expect(renderer.color(0)[0]==0 && renderer.color(1)[0]==0 && renderer.depth()[0]==depth_max,"reset initialized contents");
    });
    run_case("same-row overlap copies have snapshot semantics", [] {
        for (bool right:std::array<bool,2>{{false,true}}) {
            Renderer renderer;Image image(5,1);
            for (int x=0;x<5;++x) image.at(x,0)=static_cast<std::uint16_t>(x+1);
            renderer.copy_image(image,1,{0,0},{0,0},5,1);
            renderer.copy_rect(1,1,{right?0:1,0},{right?1:0,0},4,1);
            const std::array<std::uint16_t,5> expected=right?std::array<std::uint16_t,5>{{1,1,2,3,4}}:std::array<std::uint16_t,5>{{2,3,4,5,5}};
            for (int x=0;x<5;++x) expect(renderer.color(1)[x]==expected[x],"horizontal overlap snapshot");
        }
    });
    run_case("vertical overlap and clipping preserve correspondence", [] {
        Renderer renderer;Image image(3,4);
        for (int y=0;y<4;++y) for (int x=0;x<3;++x) image.at(x,y)=static_cast<std::uint16_t>(3*y+x+1);
        renderer.copy_image(image,1,{0,0},{0,0},3,4);
        renderer.copy_rect(1,1,{0,0},{0,1},3,3);
        expect(renderer.color(1)[pixel_index(0,1)]==1 && renderer.color(1)[pixel_index(0,3)]==7,"vertical overlap snapshot");
        renderer.copy_image(image,1,{0,0},{-1,-1},3,2);
        expect(renderer.color(1)[0]==5 && renderer.color(1)[1]==6,"source advances with negative destination clipping");
    });
    run_case("front-source copies preserve shared depth", [] {
        Renderer renderer;Pass pass;pass.clear_color=0x07e0u;pass.clear_depth=321;renderer.render(pass);
        renderer.queue_present(1);renderer.vertical_blank();
        renderer.copy_rect(1,0,{0,0},{0,0},screen_width,screen_height);
        expect(renderer.color(0)==renderer.color(1),"front may be read by copy");
        expect(renderer.depth()[0]==321,"copy never changes depth");
        expect_throws([&] { renderer.copy_rect(1,0,{0,0},{0,0},-1,2); },"negative copy dimensions rejected");
    });
    run_case("invalid primitive fault requires attachment reinitialization", [] {
        Renderer renderer;Pass bad;
        Sprite sprite;sprite.bounds={0,0,2,2};sprite.state.texture=solid_texture(0xffffu);
        bad.primitives.emplace_back(sprite);
        expect_throws([&] { renderer.render(bad); },"sprite source extent validated");
        expect(!renderer.color_valid(1) && !renderer.depth_valid(),"failed execution invalidates writable attachments");
        expect(renderer.front_color()[0]==0,"fault preserves front");
        Pass load;load.color_load=LoadOp::Load;load.depth_load=LoadOp::Load;
        expect_throws([&] { renderer.render(load); },"invalid attachment LOAD rejected");
        Pass clear;renderer.render(clear);
        expect(renderer.color_valid(1) && renderer.depth_valid(),"full CLEAR restores validity");
    });
}

Pass mixed_scene() {
    Pass pass;pass.clear_color=0x18e3u;
    auto texture=solid_texture(0,8,8);
    for (int y=0;y<8;++y) for (int x=0;x<8;++x)
        texture->at(x,y)=pack_rgb565(Color{static_cast<std::uint8_t>(x*32),static_cast<std::uint8_t>(y*32),static_cast<std::uint8_t>((x+y)*16)});
    std::mt19937 random(0x235020);
    auto integer=[&](int low,int high) {return std::uniform_int_distribution<int>(low,high)(random);};
    for (int i=0;i<36;++i) {
        const int x=integer(0,590),y=integer(0,430),width=integer(2,48),height=integer(2,48);
        std::array<ScreenVertex,3> v{};
        v[0].x=x+integer(0,15)/16.0L;v[0].y=y+integer(0,15)/16.0L;
        v[1].x=x+width;v[1].y=y+integer(0,height/2);
        v[2].x=x+integer(0,width/2);v[2].y=y+height;
        for (auto& p:v) {
            p.depth=integer(1,1000)/1001.0L;p.w=1<<integer(0,3);
            p.u=integer(-8,16);p.v=integer(-8,16);p.r=integer(0,255);p.g=integer(0,255);p.b=integer(0,255);
        }
        RenderState state;state.shade=static_cast<ShadeMode>(i%5);state.texture=texture;
        state.address_u=AddressMode::Repeat;state.address_v=i%2?AddressMode::Repeat:AddressMode::Clamp;
        state.color={static_cast<std::uint8_t>(integer(0,255)),static_cast<std::uint8_t>(integer(0,255)),static_cast<std::uint8_t>(integer(0,255))};
        state.depth_test=i%6!=0;state.depth_write=i%5!=0;state.color_write=i%7!=0;
        if (i%4==0) state.scissor={x+1,y+1,x+width,y+height};
        if (auto t=setup_triangle(v,state)) pass.primitives.emplace_back(*t);
    }
    for (int i=0;i<16;++i) {
        const int x=integer(0,620),y=integer(0,460);
        auto rect=rectangle({x,y,x+integer(1,20),y+integer(1,20)},
            Color{static_cast<std::uint8_t>(integer(0,255)),static_cast<std::uint8_t>(integer(0,255)),static_cast<std::uint8_t>(integer(0,255))},integer(1,depth_max),true);
        pass.primitives.emplace_back(rect);
        Line line;line.a={integer(0,639),integer(0,479)};line.b={integer(0,639),integer(0,479)};
        line.state=overlay_state(Color{255,255,0});pass.primitives.emplace_back(line);
        Sprite sprite;sprite.bounds={x,y,x+4,y+4};sprite.source_x=integer(0,4);sprite.source_y=integer(0,4);
        sprite.state.texture=texture;if (i%2) sprite.state.color_key=texture->at(0,0);pass.primitives.emplace_back(sprite);
    }
    return pass;
}

void traversal_tests() {
    run_case("seeded mixed scene is independent of tile traversal", [] {expect_modes_equal(mixed_scene());});
    run_case("large perspective planes survive every tile boundary", [] {
        std::array<ScreenVertex,3> vertices{};
        vertices[0].x=0;vertices[0].y=0;vertices[0].w=1;vertices[0].u=-1024;vertices[0].v=-1024;vertices[0].depth=0.1L;
        vertices[1].x=640;vertices[1].y=0;vertices[1].w=64;vertices[1].u=1024;vertices[1].v=-1024;vertices[1].depth=0.9L;
        vertices[2].x=0;vertices[2].y=480;vertices[2].w=8;vertices[2].u=-1024;vertices[2].v=1024;vertices[2].depth=0.5L;
        auto texture=solid_texture(0,8,8);
        for (int y=0;y<8;++y) for (int x=0;x<8;++x) texture->at(x,y)=static_cast<std::uint16_t>(1+8*y+x);
        RenderState state;state.shade=ShadeMode::TextureReplace;state.texture=texture;state.address_u=AddressMode::Repeat;state.address_v=AddressMode::Repeat;
        Pass pass;pass.primitives.emplace_back(require_triangle(vertices,state));expect_modes_equal(pass);
    });
    run_case("equal-depth order and global lines are tile invariant", [] {
        Pass pass;
        pass.primitives.emplace_back(rectangle({0,0,640,480},{255,0,0},100,true));
        auto same=rectangle({20,20,620,460},{0,255,0},100,true);pass.primitives.emplace_back(same);
        same.state.depth_func=DepthFunc::LessEqual;same.bounds={31,31,609,449};same.state.color={0,0,255};pass.primitives.emplace_back(same);
        for (const auto& endpoints:std::array<std::array<Point,2>,3>{{{{{0,0},{639,479}}},{{{0,479},{639,0}}},{{{31,0},{32,479}}}}}) {
            Line line;line.a=endpoints[0];line.b=endpoints[1];line.state=overlay_state(Color{255,255,255});pass.primitives.emplace_back(line);
        }
        expect_modes_equal(pass);
    });
    run_case("multipass LOAD results match all traversal modes", [] {
        Renderer tiled,reverse,mono;
        const auto first=mixed_scene();
        tiled.render(first,RenderMode::Tiled);reverse.render(first,RenderMode::TiledReverse);mono.render(first,RenderMode::Monolithic);
        Pass second;second.color_load=LoadOp::Load;second.depth_load=LoadOp::Load;
        second.primitives.emplace_back(rectangle({31,31,65,65},{0,255,255}));
        Line line;line.a={0,470};line.b={639,10};line.state=overlay_state(Color{255,0,255});second.primitives.emplace_back(line);
        tiled.render(second,RenderMode::Tiled);reverse.render(second,RenderMode::TiledReverse);mono.render(second,RenderMode::Monolithic);
        expect_buffers_equal(tiled,reverse,"multipass reverse order");expect_buffers_equal(tiled,mono,"multipass monolithic");
    });
}

} // namespace

int main() {
    golden_tests();
    numeric_tests();
    shading_tests();
    depth_and_pass_tests();
    copy_and_ownership_tests();
    traversal_tests();
    std::cout << cases_run-cases_failed << '/' << cases_run << " reference cases passed\n";
    return cases_failed==0?0:1;
}
